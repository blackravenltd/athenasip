//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/resolver.h"

#include <openssl/rand.h>

#include <boost/asio.hpp>
#include <fstream>
#include <sstream>

#include "global_io_context.h"
#include "loggers/logger_scoped.h"
#include "util.h"

namespace athenasip::dns {

namespace {

// resolv.conf(5)'s default for "attempts".
constexpr std::size_t kAttempts = 2;

// How long an empty answer is cached. RFC 2308 takes this from the SOA, which this resolver does not keep.
constexpr std::chrono::seconds kNegativeTtl{30};

// Past this the expired entries go, and if that is not enough, all of them.
constexpr std::size_t kCacheLimit = 4096;

// RFC 5452 section 4: a random id, because it is what an off-path attacker has to guess.
std::uint16_t random_id() {
  std::uint16_t id = 0;
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&id), sizeof(id)) != 1) id = static_cast<std::uint16_t>(std::rand());
  return id;
}

}  // namespace

struct UdpResolver::Attempt {
  // Everything about one query runs on this strand: the io_context is shared between threads.
  boost::asio::any_io_executor strand;

  plugins::Executor on;
  std::string name;
  Type type = Type::A;
  plugins::Handler<std::vector<Record>> handler;

  std::size_t server = 0;
  std::size_t round = 0;
  std::uint16_t id = 0;
  std::vector<std::uint8_t> query;

  std::shared_ptr<boost::asio::ip::udp::socket> socket;
  std::shared_ptr<boost::asio::ip::tcp::socket> stream;
  std::shared_ptr<boost::asio::steady_timer> timer;
  std::vector<std::uint8_t> buffer;
  boost::asio::ip::udp::endpoint from;

  bool done = false;
};

UdpResolver::UdpResolver(std::shared_ptr<loggers::Logger> logger, std::vector<boost::asio::ip::udp::endpoint> servers, std::chrono::milliseconds timeout)
    : _logger(std::make_shared<loggers::LoggerScoped>("dns", std::move(logger))), _servers(std::move(servers)), _timeout(timeout) {}

std::vector<boost::asio::ip::udp::endpoint> UdpResolver::servers_from(const std::string& resolv_conf) {
  std::vector<boost::asio::ip::udp::endpoint> servers;

  std::ifstream file(resolv_conf);
  std::string line;

  while (std::getline(file, line)) {
    const auto comment = line.find_first_of("#;");
    if (comment != std::string::npos) line.erase(comment);

    std::istringstream words(line);
    std::string keyword;
    std::string value;
    if (!(words >> keyword >> value) || keyword != "nameserver") continue;

    // A link-local IPv6 server is written with its scope, "fe80::1%en0".
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(value, ec);
    if (!ec) servers.emplace_back(address, 53);
  }

  return servers;
}

void UdpResolver::query(plugins::Executor on, std::string name, Type type, plugins::Handler<std::vector<Record>> handler) {
  auto attempt = std::make_shared<Attempt>();
  attempt->on = std::move(on);
  attempt->name = std::move(name);
  attempt->type = type;
  attempt->handler = std::move(handler);
  attempt->strand = boost::asio::make_strand(detail::get_global_io_context());

  std::vector<Record> cached;
  if (_cached(_cache_key(attempt->name, attempt->type), cached)) {
    attempt->done = true;
    boost::asio::post(attempt->on, [handler = attempt->handler, cached = std::move(cached)]() mutable {
      handler(plugins::Result<std::vector<Record>>::success(std::move(cached)));
    });
    return;
  }

  if (_servers.empty()) return _finish(attempt, plugins::Result<std::vector<Record>>::failure("no nameservers configured"));

  auto self = shared_from_this();
  boost::asio::dispatch(attempt->strand, [self, attempt]() { self->_ask(attempt); });
}

void UdpResolver::_ask(std::shared_ptr<Attempt> attempt) {
  auto& io = attempt->strand;
  const auto& server = _servers[attempt->server];

  attempt->id = random_id();
  attempt->query = encode_query(attempt->id, attempt->name, attempt->type);

  boost::system::error_code ec;
  attempt->socket = std::make_shared<boost::asio::ip::udp::socket>(io);
  attempt->socket->open(server.protocol(), ec);
  if (ec) return _next(attempt, "cannot open a socket - " + ec.message());

  attempt->timer = std::make_shared<boost::asio::steady_timer>(io);
  attempt->timer->expires_after(_timeout);

  auto self = shared_from_this();
  auto socket = attempt->socket;

  attempt->timer->async_wait([self, attempt, socket](const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted || attempt->socket != socket) return;
    self->_next(attempt, "no answer in time");
  });

  attempt->socket->async_send_to(boost::asio::buffer(attempt->query), server, [self, attempt, socket](const boost::system::error_code& ec, std::size_t) {
    if (attempt->socket != socket) return;
    if (ec) return self->_next(attempt, "cannot send - " + ec.message());

    attempt->buffer.assign(65535, 0);

    // A function of its own, because a reply that is not ours goes back to listening.
    auto listen = std::make_shared<std::function<void()>>();
    *listen = [self, attempt, socket, listen]() {
      socket->async_receive_from(
          boost::asio::buffer(attempt->buffer), attempt->from, [self, attempt, socket, listen](const boost::system::error_code& ec, std::size_t length) {
            if (attempt->socket != socket || attempt->done) {
              *listen = nullptr;
              return;
            }
            if (ec) {
              *listen = nullptr;
              return self->_next(attempt, "cannot receive - " + ec.message());
            }

            const std::vector<std::uint8_t> message(attempt->buffer.begin(), attempt->buffer.begin() + static_cast<std::ptrdiff_t>(length));
            const auto response = decode_response(message);

            // RFC 5452: the reply must come from the server asked and carry the id sent; the socket's port is fresh
            // per query. Anything else is ignored and the socket keeps listening.
            const auto& server = self->_servers[attempt->server];
            if (attempt->from != server || !response || response->id != attempt->id) return (*listen)();

            attempt->timer->cancel();
            *listen = nullptr;

            if (response->truncated) return self->_ask_tcp(attempt);

            // 2 is SERVFAIL and 5 REFUSED: the next server might answer.
            if (response->rcode == 2 || response->rcode == 5) {
              return self->_next(attempt, "server said rcode " + std::to_string(response->rcode));
            }

            std::vector<Record> records;
            for (const auto& record : response->answers) {
              if (record.type == attempt->type) records.push_back(record);
            }

            // 0, with or without answers, and 3 NXDOMAIN are both answers.
            self->_finish(attempt, plugins::Result<std::vector<Record>>::success(std::move(records)));
          });
    };
    (*listen)();
  });
}

// RFC 1035 4.2.2: the same message over TCP, prefixed with its length.
void UdpResolver::_ask_tcp(std::shared_ptr<Attempt> attempt) {
  auto& io = attempt->strand;
  const auto& server = _servers[attempt->server];

  attempt->socket.reset();
  attempt->stream = std::make_shared<boost::asio::ip::tcp::socket>(io);
  attempt->timer = std::make_shared<boost::asio::steady_timer>(io);
  attempt->timer->expires_after(_timeout);

  auto self = shared_from_this();
  auto stream = attempt->stream;

  attempt->timer->async_wait([self, attempt, stream](const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted || attempt->stream != stream) return;
    boost::system::error_code ignored;
    stream->close(ignored);
    self->_next(attempt, "no answer over TCP in time");
  });

  auto framed = std::make_shared<std::vector<std::uint8_t>>();
  framed->push_back(static_cast<std::uint8_t>(attempt->query.size() >> 8));
  framed->push_back(static_cast<std::uint8_t>(attempt->query.size() & 0xff));
  framed->insert(framed->end(), attempt->query.begin(), attempt->query.end());

  const boost::asio::ip::tcp::endpoint target(server.address(), server.port());

  stream->async_connect(target, [self, attempt, stream, framed](const boost::system::error_code& ec) {
    if (attempt->stream != stream || attempt->done) return;
    if (ec) return self->_next(attempt, "cannot reach over TCP - " + ec.message());

    boost::asio::async_write(*stream, boost::asio::buffer(*framed), [self, attempt, stream, framed](const boost::system::error_code& ec, std::size_t) {
      if (attempt->stream != stream || attempt->done) return;
      if (ec) return self->_next(attempt, "cannot send over TCP - " + ec.message());

      attempt->buffer.assign(2, 0);
      boost::asio::async_read(*stream, boost::asio::buffer(attempt->buffer), [self, attempt, stream](const boost::system::error_code& ec, std::size_t) {
        if (attempt->stream != stream || attempt->done) return;
        if (ec) return self->_next(attempt, "cannot read over TCP - " + ec.message());

        const std::size_t length = (static_cast<std::size_t>(attempt->buffer[0]) << 8) | attempt->buffer[1];
        attempt->buffer.assign(length, 0);

        boost::asio::async_read(*stream, boost::asio::buffer(attempt->buffer), [self, attempt, stream](const boost::system::error_code& ec, std::size_t) {
          if (attempt->stream != stream || attempt->done) return;
          if (ec) return self->_next(attempt, "cannot read over TCP - " + ec.message());

          attempt->timer->cancel();

          const auto response = decode_response(attempt->buffer);
          if (!response || response->id != attempt->id) return self->_next(attempt, "a TCP answer that is not ours");
          if (response->rcode == 2 || response->rcode == 5) return self->_next(attempt, "server said rcode " + std::to_string(response->rcode));

          std::vector<Record> records;
          for (const auto& record : response->answers) {
            if (record.type == attempt->type) records.push_back(record);
          }
          self->_finish(attempt, plugins::Result<std::vector<Record>>::success(std::move(records)));
        });
      });
    });
  });
}

void UdpResolver::_next(std::shared_ptr<Attempt> attempt, const std::string& why) {
  if (attempt->done) return;

  _logger->debug(attempt->name + " from " + _servers[attempt->server].address().to_string() + ": " + why);

  if (attempt->timer) attempt->timer->cancel();

  boost::system::error_code ignored;
  if (attempt->socket) attempt->socket->close(ignored);
  if (attempt->stream) attempt->stream->close(ignored);
  attempt->socket.reset();
  attempt->stream.reset();

  // resolv.conf(5): every server in turn, then round again, kAttempts times in all.
  if (++attempt->server >= _servers.size()) {
    attempt->server = 0;
    if (++attempt->round >= kAttempts) {
      return _finish(attempt, plugins::Result<std::vector<Record>>::failure("no nameserver answered for " + attempt->name));
    }
  }

  _ask(attempt);
}

void UdpResolver::_finish(std::shared_ptr<Attempt> attempt, plugins::Result<std::vector<Record>> result) {
  if (attempt->done) return;
  attempt->done = true;

  // Only an answer is cached, never a failure.
  if (result.ok) _keep(_cache_key(attempt->name, attempt->type), result.value);

  if (attempt->timer) attempt->timer->cancel();

  boost::system::error_code ignored;
  if (attempt->socket) attempt->socket->close(ignored);
  if (attempt->stream) attempt->stream->close(ignored);

  boost::asio::post(attempt->on, [handler = attempt->handler, result = std::move(result)]() mutable { handler(std::move(result)); });
}

// RFC 4343: names compare case-insensitively. A trailing dot is dropped.
std::string UdpResolver::_cache_key(const std::string& name, Type type) {
  auto key = Util::to_lower(name);
  if (!key.empty() && key.back() == '.') key.pop_back();
  return key + "/" + std::to_string(static_cast<int>(type));
}

bool UdpResolver::_cached(const std::string& key, std::vector<Record>& out) {
  std::lock_guard lock(_cache_mutex);

  auto found = _cache.find(key);
  if (found == _cache.end()) return false;

  if (found->second.expires <= std::chrono::steady_clock::now()) {
    _cache.erase(found);
    return false;
  }

  out = found->second.records;
  return true;
}

void UdpResolver::_keep(const std::string& key, const std::vector<Record>& records) {
  // The set is good for its shortest TTL. Zero means do not keep.
  std::chrono::seconds ttl = kNegativeTtl;
  if (!records.empty()) {
    std::uint32_t shortest = records.front().ttl;
    for (const auto& record : records) shortest = std::min(shortest, record.ttl);
    ttl = std::chrono::seconds(shortest);
  }
  if (ttl.count() == 0) return;

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(_cache_mutex);

  if (_cache.size() >= kCacheLimit) {
    std::erase_if(_cache, [now](const auto& entry) { return entry.second.expires <= now; });
    if (_cache.size() >= kCacheLimit) _cache.clear();
  }

  _cache[key] = Cached{records, now + ttl};
}

}  // namespace athenasip::dns
