//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "address_discovery.h"

#include <openssl/rand.h>

#include <boost/asio/post.hpp>
#include <chrono>
#include <utility>

#include "channel.h"
#include "core.h"
#include "util.h"

namespace athenasip {

namespace {

// RFC 5389 7.2.1 retransmits for 39.5 seconds; one server not answering in three is enough to try the next.
constexpr auto kAnswerWithin = std::chrono::seconds(3);

}  // namespace

AddressDiscovery::AddressDiscovery(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("address", std::move(logger))), _core(std::move(core)) {}

std::vector<std::pair<std::string, std::uint16_t>> AddressDiscovery::servers_from(const std::vector<std::string>& urls) {
  std::vector<std::pair<std::string, std::uint16_t>> servers;

  for (const auto& url : urls) {
    if (Util::to_lower(url.substr(0, 5)) != "stun:") continue;

    // RFC 7064: stun:host[:port], the host possibly an IPv6 literal in brackets.
    auto rest = url.substr(5);
    if (const auto query = rest.find('?'); query != std::string::npos) rest = rest.substr(0, query);

    std::string host = rest;
    std::uint16_t port = 3478;

    const auto bracket = rest.find(']');
    const auto colon = rest.rfind(':');
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
      host = rest.substr(0, colon);
      try {
        port = static_cast<std::uint16_t>(std::stoul(rest.substr(colon + 1)));
      } catch (const std::exception&) {
        continue;
      }
    }

    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (!host.empty()) servers.emplace_back(host, port);
  }

  return servers;
}

void AddressDiscovery::start(std::chrono::seconds interval) {
  auto core = _core.lock();
  if (!core) return;

  std::vector<std::string> urls;
  for (const auto& server : core->config->ice_servers) urls.push_back(server.url);

  _servers = servers_from(urls);
  _interval = interval;
  _stopped = false;

  if (_servers.empty() || !core->config->udp_enable) return;
  _ask(0);
}

void AddressDiscovery::stop() {
  _stopped = true;
  if (_timer) _timer->cancel();
  _timer.reset();
  _pending.clear();
}

void AddressDiscovery::_ask(std::size_t index) {
  auto core = _core.lock();
  if (!core || _stopped) return;

  auto timers = core->timer_source();
  if (!timers) return;

  std::weak_ptr<AddressDiscovery> weak_self = weak_from_this();
  auto strand = core->strand();
  auto later = [weak_self, strand](std::size_t next) {
    return [weak_self, strand, next]() {
      boost::asio::post(strand, [weak_self, next]() {
        if (auto self = weak_self.lock()) self->_ask(next);
      });
    };
  };

  // Every server asked this round: ask again after the interval, from the first.
  if (index >= _servers.size()) {
    _timer = timers->schedule(std::chrono::duration_cast<std::chrono::milliseconds>(_interval), later(0));
    return;
  }

  const auto [host, port] = _servers[index];
  const auto source = "stun:" + host + ":" + std::to_string(port);

  std::string transaction(12, '\0');
  RAND_bytes(reinterpret_cast<unsigned char*>(transaction.data()), static_cast<int>(transaction.size()));

  auto self = shared_from_this();

  // Sent from the SIP UDP socket: its mapping is the one clients and peers reach.
  core->channel_connect("udp", host, port, [this, self, index, source, transaction, timers](plugins::Result<std::shared_ptr<Channel>> opened) {
    if (_stopped) return;

    if (!opened.ok || !opened.value) {
      _logger->info("Cannot ask " + source + " this node's address - " + opened.error);
      return _ask(index + 1);
    }

    _pending[transaction] = source;
    opened.value->write(stun::binding_request(transaction));

    // An unanswered request is given up, and the next server asked.
    std::weak_ptr<AddressDiscovery> weak_self = weak_from_this();
    auto core = _core.lock();
    if (!core) return;
    _timer = timers->schedule(kAnswerWithin, [weak_self, strand = core->strand(), transaction, index]() {
      boost::asio::post(strand, [weak_self, transaction, index]() {
        auto self = weak_self.lock();
        if (self && self->_pending.erase(transaction) != 0) self->_ask(index + 1);
      });
    });
  });
}

void AddressDiscovery::answered(const stun::Mapped& mapped) {
  auto found = _pending.find(mapped.transaction_id);
  if (found == _pending.end()) return;

  const Finding finding{mapped.address.to_string(), mapped.port, found->second};
  _pending.erase(found);

  if (!_finding || _finding->address != finding.address || _finding->port != finding.port) {
    _logger->info(finding.source + " sees this node at " + finding.address + ":" + std::to_string(finding.port));
  }
  _finding = finding;

  // The round is done: ask again after the interval.
  if (_timer) _timer->cancel();
  _ask(_servers.size());
}

}  // namespace athenasip
