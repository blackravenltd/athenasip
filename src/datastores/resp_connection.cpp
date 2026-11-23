//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "resp_connection.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/errc.hpp>
#include <utility>

namespace athenasip::datastores::resp {

namespace {

boost::system::error_code not_connected() { return boost::system::errc::make_error_code(boost::system::errc::not_connected); }
boost::system::error_code timed_out() { return boost::system::errc::make_error_code(boost::system::errc::timed_out); }
boost::system::error_code aborted() { return boost::system::errc::make_error_code(boost::system::errc::operation_canceled); }

}  // namespace

Connection::Connection(boost::asio::io_context& io, std::shared_ptr<loggers::Logger> logger, Settings settings)
    : _io(io),
      _logger(std::make_shared<loggers::LoggerScoped>("redis connection", std::move(logger))),
      _settings(std::move(settings)),
      _resolver(io),
      _tls_context(boost::asio::ssl::context::tls_client),
      _connect_timer(io),
      _reconnect_timer(io),
      _backoff(_settings.reconnect_min),
      _in(16 * 1024) {
  if (_settings.tls) {
    // Checks who answers, as mqtts:// does: the server's certificate against the system's roots and ca_file, and its
    // name against the host it was asked for.
    _tls_context.set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_tlsv1 | boost::asio::ssl::context::no_tlsv1_1);
    _tls_context.set_default_verify_paths();
    if (!_settings.ca_file.empty()) {
      boost::system::error_code failed;
      _tls_context.load_verify_file(_settings.ca_file, failed);
      if (failed) _logger->error("redis.ca_file: could not load " + _settings.ca_file + " - " + failed.message());
    }
    _tls_context.set_verify_mode(boost::asio::ssl::verify_peer);
  }
}

Connection::~Connection() = default;

void Connection::start() {
  boost::asio::post(_io, [self = shared_from_this()]() {
    if (self->_state != State::Stopped) return;
    self->_state = State::Connecting;
    self->_connect();
  });
}

void Connection::stop() {
  boost::asio::post(_io, [self = shared_from_this()]() {
    self->_state = State::Stopped;
    self->_reconnect_timer.cancel();
    self->_connect_timer.cancel();
    self->_resolver.cancel();
    self->_close_transport();

    auto all = std::move(self->_inflight);
    for (auto& pending : self->_waiting) all.push_back(pending);
    self->_waiting.clear();
    for (const auto& pending : all) self->_answer(pending, aborted(), "the connection was closed");
  });
}

void Connection::exec(Request request, Handler handler) {
  auto pending = std::make_shared<Pending>();
  pending->request = std::move(request);
  pending->handler = std::move(handler);

  boost::asio::post(_io, [self = shared_from_this(), pending]() {
    if (self->_state == State::Stopped) return self->_answer(pending, not_connected(), "the connection is closed");
    if (self->_waiting.size() >= self->_settings.max_waiting) {
      return self->_answer(pending, not_connected(), "too many requests waiting for the link to come back");
    }

    self->_arm_deadline(pending);
    self->_waiting.push_back(pending);
    if (self->_state == State::Ready) self->_write();
  });
}

void Connection::_arm_deadline(const PendingPtr& pending) {
  pending->deadline = std::make_shared<boost::asio::steady_timer>(_io, _settings.request_timeout);
  std::weak_ptr<Pending> weak = pending;
  pending->deadline->async_wait([self = shared_from_this(), weak](boost::system::error_code ec) {
    if (ec) return;
    auto pending = weak.lock();
    if (!pending || pending->answered) return;

    // Not yet written, it simply leaves the queue; written, it keeps its place so the replies after it stay in step.
    if (auto it = std::find(self->_waiting.begin(), self->_waiting.end(), pending); it != self->_waiting.end()) self->_waiting.erase(it);
    self->_answer(pending, timed_out(), "no answer within " + std::to_string(self->_settings.request_timeout.count()) + " ms");
  });
}

void Connection::_answer(const PendingPtr& pending, boost::system::error_code ec, const std::string& what) {
  if (pending->answered) return;
  pending->answered = true;
  if (pending->deadline) pending->deadline->cancel();
  auto handler = std::move(pending->handler);
  auto replies = std::move(pending->replies);
  if (handler) handler(ec, what, ec ? std::vector<Reply>{} : std::move(replies));
}

void Connection::_connect() {
  const auto generation = ++_generation;
  _decoder = Decoder();

  _connect_timer.expires_after(_settings.connect_timeout);
  _connect_timer.async_wait([self = shared_from_this(), generation](boost::system::error_code ec) {
    if (ec || generation != self->_generation || self->_state == State::Ready || self->_state == State::Stopped) return;
    self->_lost("no connection within " + std::to_string(self->_settings.connect_timeout.count()) + " ms");
  });

  _resolver.async_resolve(_settings.host, std::to_string(_settings.port),
                          [self = shared_from_this(), generation](boost::system::error_code ec, boost::asio::ip::tcp::resolver::results_type results) {
                            if (generation != self->_generation || self->_state == State::Stopped) return;
                            if (ec) return self->_lost("cannot resolve " + self->_settings.host + " - " + ec.message());

                            auto socket = std::make_unique<boost::asio::ip::tcp::socket>(self->_io);
                            auto* raw = socket.get();
                            self->_socket = std::move(socket);
                            boost::asio::async_connect(*raw, results, [self, generation](boost::system::error_code ec, const auto&) {
                              if (generation != self->_generation || self->_state == State::Stopped) return;
                              if (ec)
                                return self->_lost("cannot reach " + self->_settings.host + ":" + std::to_string(self->_settings.port) + " - " + ec.message());
                              self->_on_connected();
                            });
                          });
}

void Connection::_on_connected() {
  const auto generation = _generation;
  boost::system::error_code ignored;
  _socket->set_option(boost::asio::socket_base::keep_alive(true), ignored);
  _socket->set_option(boost::asio::ip::tcp::no_delay(true), ignored);

  if (!_settings.tls) return _handshake();

  _tls = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*_socket), _tls_context);
  _socket.reset();
  SSL_set_tlsext_host_name(_tls->native_handle(), _settings.host.c_str());
  _tls->set_verify_callback(boost::asio::ssl::host_name_verification(_settings.host));
  _tls->async_handshake(boost::asio::ssl::stream_base::client, [self = shared_from_this(), generation](boost::system::error_code ec) {
    if (generation != self->_generation || self->_state == State::Stopped) return;
    if (ec) return self->_lost("TLS to " + self->_settings.host + " failed - " + ec.message());
    self->_handshake();
  });
}

// AUTH and SELECT go first, ahead of anything waiting, and the link is ready only once both are answered.
void Connection::_handshake() {
  _state = State::Handshaking;
  _read();

  Request request;
  if (!_settings.password.empty()) {
    if (_settings.username.empty() || _settings.username == "default") {
      request.push("AUTH", _settings.password);
    } else {
      request.push("AUTH", _settings.username, _settings.password);
    }
  }
  if (_settings.database != 0) request.push("SELECT", _settings.database);

  if (request.commands() == 0) return _on_ready();

  auto pending = std::make_shared<Pending>();
  pending->request = std::move(request);
  pending->internal = true;
  const auto generation = _generation;
  pending->handler = [self = shared_from_this(), generation](boost::system::error_code ec, std::string what, std::vector<Reply> replies) {
    if (generation != self->_generation || self->_state == State::Stopped) return;
    if (ec) return self->_lost("signing in failed - " + what);
    for (const auto& reply : replies) {
      if (reply.is_error()) {
        self->_logger->error("Redis refused the sign-in: " + reply.text);
        return self->_lost("Redis refused the sign-in: " + reply.text);
      }
    }
    self->_on_ready();
  };
  _arm_deadline(pending);

  // Written now, ahead of the queue, which waits for Ready.
  _inflight.push_back(pending);
  _out = pending->request.payload();
  _writing = true;
  _async_write(boost::asio::buffer(_out), [self = shared_from_this(), generation](boost::system::error_code ec, std::size_t) {
    if (generation != self->_generation || self->_state == State::Stopped) return;
    self->_writing = false;
    if (ec) return self->_lost("cannot write to Redis - " + ec.message());
    if (self->_state == State::Ready) self->_write();
  });
}

void Connection::_on_ready() {
  _state = State::Ready;
  _backoff = _settings.reconnect_min;
  _connect_timer.cancel();
  _logger->info("Connected to Redis at " + _settings.host + ":" + std::to_string(_settings.port) + (_settings.tls ? " over TLS" : ""));
  _write();
}

void Connection::_write() {
  if (_writing || _state != State::Ready || _waiting.empty()) return;

  _out.clear();
  while (!_waiting.empty()) {
    auto pending = std::move(_waiting.front());
    _waiting.pop_front();
    if (pending->answered) continue;
    _out += pending->request.payload();
    _inflight.push_back(std::move(pending));
  }
  if (_out.empty()) return;

  _writing = true;
  const auto generation = _generation;
  _async_write(boost::asio::buffer(_out), [self = shared_from_this(), generation](boost::system::error_code ec, std::size_t) {
    if (generation != self->_generation || self->_state == State::Stopped) return;
    self->_writing = false;
    if (ec) return self->_lost("cannot write to Redis - " + ec.message());
    self->_write();
  });
}

void Connection::_read() {
  const auto generation = _generation;
  _async_read_some(boost::asio::buffer(_in), [self = shared_from_this(), generation](boost::system::error_code ec, std::size_t length) {
    if (generation != self->_generation || self->_state == State::Stopped) return;
    if (ec) return self->_lost(ec == boost::asio::error::eof ? "Redis closed the connection" : "cannot read from Redis - " + ec.message());

    self->_decoder.feed(std::string_view(self->_in.data(), length));
    try {
      while (auto reply = self->_decoder.next()) {
        self->_on_reply(std::move(*reply));
        if (generation != self->_generation) return;
      }
    } catch (const ProtocolError& error) {
      self->_logger->error(std::string("Redis sent what is not RESP: ") + error.what());
      return self->_lost(error.what());
    }
    self->_read();
  });
}

void Connection::_on_reply(Reply reply) {
  if (_inflight.empty()) return _lost("a reply nobody asked for");

  auto pending = _inflight.front();
  pending->replies.push_back(std::move(reply));
  if (pending->replies.size() < pending->request.commands()) return;

  _inflight.pop_front();
  _answer(pending, {}, {});
}

// The link has gone, or never came: what was written fails, since it may or may not have been carried out; what was
// not yet written waits, within its deadline, for the next link.
void Connection::_lost(const std::string& why) {
  if (_state == State::Stopped) return;

  if (_state == State::Ready)
    _logger->warn("Lost the connection to Redis: " + why);
  else
    _logger->debug("No connection to Redis: " + why);

  ++_generation;
  _close_transport();
  _connect_timer.cancel();
  _writing = false;

  auto written = std::move(_inflight);
  _inflight.clear();
  for (const auto& pending : written) {
    if (pending->internal) continue;
    _answer(pending, not_connected(), why);
  }

  _state = State::Waiting;
  _schedule_reconnect();
}

void Connection::_schedule_reconnect() {
  // Up to a quarter either way, so nodes that lost the same server do not all come back at once.
  std::uniform_int_distribution<long long> spread(-_backoff.count() / 4, _backoff.count() / 4);
  const auto wait = std::chrono::milliseconds(_backoff.count() + spread(_jitter));
  _backoff = std::min(_backoff * 2, _settings.reconnect_max);

  _reconnect_timer.expires_after(wait);
  _reconnect_timer.async_wait([self = shared_from_this()](boost::system::error_code ec) {
    if (ec || self->_state != State::Waiting) return;
    self->_state = State::Connecting;
    self->_connect();
  });
}

void Connection::_close_transport() {
  boost::system::error_code ignored;
  if (_tls) {
    _tls->lowest_layer().close(ignored);
    _tls.reset();
  }
  if (_socket) {
    _socket->close(ignored);
    _socket.reset();
  }
}

template <typename Buffers, typename Completion>
void Connection::_async_write(const Buffers& buffers, Completion&& handler) {
  if (_tls) return boost::asio::async_write(*_tls, buffers, std::forward<Completion>(handler));
  if (_socket) return boost::asio::async_write(*_socket, buffers, std::forward<Completion>(handler));
  boost::asio::post(_io, [handler = std::forward<Completion>(handler)]() mutable { handler(not_connected(), 0); });
}

template <typename Buffers, typename Completion>
void Connection::_async_read_some(const Buffers& buffers, Completion&& handler) {
  if (_tls) return _tls->async_read_some(buffers, std::forward<Completion>(handler));
  if (_socket) return _socket->async_read_some(buffers, std::forward<Completion>(handler));
  boost::asio::post(_io, [handler = std::forward<Completion>(handler)]() mutable { handler(not_connected(), 0); });
}

}  // namespace athenasip::datastores::resp
