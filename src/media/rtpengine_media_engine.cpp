//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "rtpengine_media_engine.h"

#include <algorithm>
#include <boost/asio/bind_executor.hpp>
#include <boost/json.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <ctime>
#include <utility>

#include "../config.h"
#include "../global_io_context.h"
#include "../util.h"

namespace athenasip::media {

namespace {

constexpr std::size_t kMaxDatagram = 65535;

unsigned positive_or(const YAML::Node& node, unsigned fallback) {
  if (!node) return fallback;

  const auto value = node.as<int>(0);
  if (value <= 0) return fallback;

  return static_cast<unsigned>(value);
}

// Separate RTCP towards a leg that is not WebRTC (demux). In an offer, accept keeps rtcp-mux on the offering leg when
// it asked for it: without it a re-offer on the same call, as after a 488, makes rtpengine drop rtcp-mux from the
// answer to that leg, and a WebRTC offerer refuses an answer without it (RFC 8834 5.1.3).
Bencode demuxed(bool answering) {
  if (answering) return Bencode::list({Bencode(std::string("demux"))});
  return Bencode::list({Bencode(std::string("demux")), Bencode(std::string("accept"))});
}

// Maps a profile onto rtpengine's flags (RFC 8838, RFC 8842, RFC 5764). With no flags rtpengine mirrors what it was
// handed, which is wrong when the two legs differ. Towards a browser DTLS is passive, because the browser starts the
// handshake, and rtcp-mux is required (RFC 5761).
void apply_profile(Bencode& command, Flags::Profile profile, bool source_wants_mux, bool answering) {
  switch (profile) {
    case Flags::Profile::WebRtc:
      command.set("ICE", Bencode(std::string("force")));
      // Offer only. As answerer to an actpass offer the engine has already gone active (RFC 5763 section 5); setting
      // passive in the answer would reset a handshake in flight and it would never complete.
      if (!answering) command.set("DTLS", Bencode(std::string("passive")));
      command.set("transport-protocol", Bencode(std::string("UDP/TLS/RTP/SAVPF")));
      command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer")), Bencode(std::string("require"))}));
      return;

    case Flags::Profile::SrtpSdes:
      // RFC 4568: keys in the description, so no handshake and no ICE. rtpengine generates the crypto attributes.
      command.set("ICE", Bencode(std::string("remove")));
      command.set("DTLS", Bencode(std::string("off")));
      command.set("transport-protocol", Bencode(std::string("RTP/SAVP")));
      command.set("rtcp-mux", demuxed(answering));
      return;

    case Flags::Profile::PlainRtp:
      command.set("ICE", Bencode(std::string("remove")));
      command.set("DTLS", Bencode(std::string("off")));
      command.set("transport-protocol", Bencode(std::string("RTP/AVP")));
      command.set("rtcp-mux", demuxed(answering));
      return;

    case Flags::Profile::Mirror:
      // rtpengine keeps what it was given. rtcp-mux is offered onward only if the sending end asked for it.
      if (source_wants_mux) command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer"))}));
      return;
  }
}

// Converts key presses between two legs that send them differently (docs/behaviour.md, dtmf). RFC 4733 events to
// and from tones in the audio is rtpengine's DTMF transcoding: telephone-event stripped from the description and
// the call always transcoded, or telephone-event offered where the far leg had none. INFO is played in by the
// node, which needs inject DTMF in the offer and the answer alike. Either leg unknown, nothing is asked for.
void apply_dtmf(Bencode& command, const Flags& flags) {
  using Dtmf = types::MediaPolicy::Dtmf;
  if (!flags.dtmf_from || !flags.dtmf_to || *flags.dtmf_from == *flags.dtmf_to) return;

  Bencode::List names;
  if (*flags.dtmf_from == Dtmf::Info || *flags.dtmf_to == Dtmf::Info) names.push_back(Bencode(std::string("inject DTMF")));

  // A leg that speaks INFO is told of key presses by INFO, not by the events or tones the other leg sent.
  if (*flags.dtmf_to == Dtmf::Info) names.push_back(Bencode(std::string("block DTMF")));

  if (*flags.dtmf_from == Dtmf::Rfc4733 && *flags.dtmf_to == Dtmf::Inband) {
    names.push_back(Bencode(std::string("always transcode")));
    command.set("codec", Bencode::dictionary({{"strip", Bencode::list({Bencode(std::string("telephone-event"))})}}));
  } else if (*flags.dtmf_from == Dtmf::Inband && *flags.dtmf_to == Dtmf::Rfc4733) {
    command.set("codec", Bencode::dictionary({{"transcode", Bencode::list({Bencode(std::string("telephone-event"))})}}));
  }

  if (!names.empty()) command.set("flags", Bencode::list(std::move(names)));
}

// host, host:port or [v6]:port.
bool parse_engine(const std::string& text, std::string& host, std::uint16_t& port) {
  host = text;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos) return false;
    host = text.substr(1, close - 1);
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') return false;
      port = static_cast<std::uint16_t>(std::stoi(text.substr(close + 2)));
    }
    return !host.empty();
  }

  if (const auto colon = text.rfind(':'); colon != std::string::npos && text.find(':') == colon) {
    host = text.substr(0, colon);
    try {
      const auto value = std::stoi(text.substr(colon + 1));
      if (value <= 0 || value > 65535) return false;
      port = static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
      return false;
    }
  }

  return !host.empty();
}

// host:port or [v6]:port to listen on, where port may be 0 for any.
bool parse_listen(const std::string& text, std::string& host, std::uint16_t& port) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon + 1 >= text.size()) return false;

  host = text.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);

  try {
    const auto value = std::stoi(text.substr(colon + 1));
    if (value < 0 || value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
  } catch (const std::exception&) {
    return false;
  }
  return !host.empty();
}

// FNV-1a: the same on every node, so nodes sharing a pool put a call on the same engine.
std::uint64_t hash_of(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

RtpengineMediaEngine::RtpengineMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("rtpengine", std::move(logger))),
      _url(std::move(url)),
      _public(_logger),
      _strand(boost::asio::make_strand(detail::get_global_io_context())),
      _receive_buffer(kMaxDatagram) {
  if (_url && !_url->host.empty()) {
    Engine engine;
    engine.host = _url->host;
    if (_url->port) engine.port = *_url->port;
    _engines.push_back(std::move(engine));
  }
}

RtpengineMediaEngine::~RtpengineMediaEngine() { close(); }

std::string RtpengineMediaEngine::name() const { return "rtpengine"; }

plugins::Settings RtpengineMediaEngine::settings() {
  using namespace plugins::define;
  return {
      section("rtpengine", "The rtpengine:// media engine, over its ng control port."),
      list("rtpengine.engines", "More engines, as host or host:port, making a pool with the URL's. A call stays on the engine it started on."),
      integer("rtpengine.timeout_ms", "500", "How long to wait for one ng reply before asking again, in milliseconds.", 1),
      integer("rtpengine.attempts", "3", "Tries per request. An engine that answers none of them leaves the pool until it answers a ping.", 1),
      integer("rtpengine.ping_interval", "10", "Seconds between pings of every engine.", 1),
      text("rtpengine.media_address", "",
           "The address rtpengine advertises. Empty leaves it to rtpengine's interfaces. A leg inside sip.localnet is given the "
           "node's local address."),
      text("rtpengine.dtmf_events", "",
           "host:port this node listens on for the key presses rtpengine hears, which become SIP INFO for a leg set to info "
           "(behaviour.dtmf). An address rtpengine can reach; port 0 picks one. Empty is off."),
  };
}

std::string RtpengineMediaEngine::version() const { return "0.0.1"; }

bool RtpengineMediaEngine::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (own_root && own_root.IsMap()) {
    _timeout_ms = positive_or(own_root["timeout_ms"], _timeout_ms);
    _attempts = positive_or(own_root["attempts"], _attempts);
    _ping_interval = positive_or(own_root["ping_interval"], _ping_interval);

    if (own_root["media_address"]) _media_address = own_root["media_address"].as<std::string>();

    if (own_root["dtmf_events"]) {
      _dtmf_events = own_root["dtmf_events"].as<std::string>("");
      std::string host;
      std::uint16_t port = 0;
      if (!_dtmf_events.empty() && !parse_listen(_dtmf_events, host, port)) {
        _logger->error("rtpengine.dtmf_events: cannot read \"" + _dtmf_events + "\" as host:port");
        return false;
      }
    }

    if (const auto engines = own_root["engines"]; engines && engines.IsSequence()) {
      for (const auto& entry : engines) {
        Engine engine;
        if (!parse_engine(entry.as<std::string>(""), engine.host, engine.port)) {
          _logger->error("rtpengine.engines: cannot read \"" + entry.as<std::string>("") + "\" as host or host:port");
          return false;
        }

        const auto name = engine.name();
        if (std::none_of(_engines.begin(), _engines.end(), [&name](const Engine& known) { return known.name() == name; })) {
          _engines.push_back(std::move(engine));
        }
      }
    }
  }

  if (_engines.empty()) {
    _logger->error("rtpengine:// needs a host, or rtpengine.engines: the engine is somewhere else by definition");
    return false;
  }

  return true;
}

Capabilities RtpengineMediaEngine::capabilities() const {
  Capabilities capabilities;
  capabilities.bridge = true;
  capabilities.record = true;
  capabilities.transcode = true;
  capabilities.dtmf = true;
  return capabilities;
}

bool RtpengineMediaEngine::is_connected() const { return _up.load(std::memory_order_relaxed) > 0; }

void RtpengineMediaEngine::_look_up(std::size_t index, std::function<void()> then) {
  // Past the last engine: carry on, on the strand.
  if (index >= _engines.size()) return boost::asio::dispatch(_strand, std::move(then));

  boost::system::error_code literal;
  boost::asio::ip::make_address(_engines[index].host, literal);
  if (!literal) return _look_up(index + 1, std::move(then));

  auto self = shared_from_this();
  auto resolver = std::make_shared<boost::asio::ip::udp::resolver>(_strand);
  const auto host = _engines[index].host;
  resolver->async_resolve(boost::asio::ip::udp::v4(), host, std::to_string(_engines[index].port),
                          [this, self, resolver, index, host, then = std::move(then)](const boost::system::error_code& ec,
                                                                                     boost::asio::ip::udp::resolver::results_type results) mutable {
                            if (ec || results.empty()) {
                              _logger->error("Could not resolve " + host + (ec ? " - " + ec.message() : std::string()));
                            } else {
                              _engines[index].found = results.begin()->endpoint();
                              _engines[index].looked_up = true;
                            }
                            _look_up(index + 1, std::move(then));
                          });
}

void RtpengineMediaEngine::connect(plugins::Executor on, plugins::StatusHandler handler) {
  if (is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

  // Resolves a name before the node serves anything.
  _public.set(_media_address);
  _public.start();

  if (_engines.empty()) {
    return _complete(std::move(on), std::move(handler), plugins::Status::failure("no rtpengine host configured"));
  }

  auto self = shared_from_this();

  _look_up(0, [this, self, on, handler = std::move(handler)]() mutable {
    std::optional<boost::asio::ip::udp> protocol;

    for (auto& engine : _engines) {
      boost::system::error_code ec;
      auto address = boost::asio::ip::make_address(engine.host, ec);

      if (ec) {
        // A name, looked up already (_look_up).
        if (!engine.looked_up) continue;
        engine.endpoint = engine.found;
      } else {
        engine.endpoint = boost::asio::ip::udp::endpoint(address, engine.port);
      }

      // One socket speaks to the whole pool, so the pool is one address family.
      if (!protocol) protocol = engine.endpoint.protocol();
      if (engine.endpoint.protocol() != *protocol) {
        _logger->error(engine.name() + " is not the same address family as " + _engines.front().name() + " - left out");
        continue;
      }

      engine.resolved = true;
    }

    if (!protocol) return _complete(std::move(on), std::move(handler), plugins::Status::failure("could not resolve any rtpengine"));

    boost::system::error_code ec;
    auto socket = std::make_shared<boost::asio::ip::udp::socket>(detail::get_global_io_context());

    socket->open(*protocol, ec);
    if (ec) {
      return _complete(std::move(on), std::move(handler), plugins::Status::failure("could not open a socket - " + ec.message()));
    }

    {
      std::lock_guard<std::mutex> lock(_socket_mutex);
      _socket = std::move(socket);
    }

    _receive();

    // Where rtpengine sends the key presses it hears, when asked to.
    if (!_dtmf_events.empty()) {
      std::string host;
      std::uint16_t port = 0;
      parse_listen(_dtmf_events, host, port);
      auto address = boost::asio::ip::make_address(host, ec);
      if (ec) {
        _logger->warn("rtpengine.dtmf_events: " + host + " is not an address - key presses will not become INFO");
      } else {
        auto events = std::make_shared<boost::asio::ip::udp::socket>(detail::get_global_io_context());
        events->open(address.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4(), ec);
        if (!ec) events->bind(boost::asio::ip::udp::endpoint(address, port), ec);
        if (ec) {
          _logger->warn("rtpengine.dtmf_events: cannot listen on " + _dtmf_events + " - " + ec.message() + " - key presses will not become INFO");
        } else {
          const auto bound = events->local_endpoint();
          _events_destination = (address.is_v6() ? "[" + address.to_string() + "]" : address.to_string()) + ":" + std::to_string(bound.port());
          {
            std::lock_guard<std::mutex> lock(_socket_mutex);
            _events_socket = std::move(events);
          }
          _events_receive();
          _logger->info("Listening for key presses rtpengine hears on " + _events_destination);
        }
      }
    }

    // A UDP socket opens whether or not anything listens, so the pool is connected only once an engine answers a ping.
    _ping_all([this, self, on, handler = std::move(handler)]() mutable {
      if (!is_connected()) {
        close();
        return _complete(std::move(on), std::move(handler), plugins::Status::failure("no rtpengine answered a ping"));
      }

      _ping_schedule();
      _complete(std::move(on), std::move(handler), plugins::Status::success());
    });
  });
}

void RtpengineMediaEngine::_ping_all(std::function<void()> done) {
  auto self = shared_from_this();
  auto outstanding = std::make_shared<std::size_t>(0);

  for (std::size_t i = 0; i < _engines.size(); ++i) {
    if (_engines[i].resolved) ++*outstanding;
  }

  if (*outstanding == 0) return done();

  for (std::size_t i = 0; i < _engines.size(); ++i) {
    if (!_engines[i].resolved) continue;

    auto ping = Bencode::dictionary({{"command", Bencode(std::string("ping"))}});

    _request(std::move(ping), i, [this, self, i, outstanding, done](std::optional<Bencode> reply) {
      const auto result = reply ? reply->string_at("result") : std::string();

      if (reply && result != "pong") _logger->warn(_engines[i].name() + " answered a ping with \"" + result + "\"");
      _mark(i, result == "pong");

      if (--*outstanding == 0) done();
    });
  }
}

void RtpengineMediaEngine::_ping_schedule() {
  auto self = shared_from_this();

  _ping_timer = std::make_shared<boost::asio::steady_timer>(_strand);
  _ping_timer->expires_after(std::chrono::seconds(_ping_interval));
  _ping_timer->async_wait(boost::asio::bind_executor(_strand, [this, self, timer = _ping_timer](const boost::system::error_code& error) {
    if (error || !_current_socket()) return;
    _ping_all([this, self]() {
      if (_current_socket()) _ping_schedule();
    });
  }));
}

void RtpengineMediaEngine::_mark(std::size_t index, bool up) {
  auto& engine = _engines[index];
  if (engine.up == up) return;

  engine.up = up;

  if (up) {
    _up.fetch_add(1, std::memory_order_relaxed);
    _logger->info("rtpengine at " + engine.host + ":" + std::to_string(engine.port) + " is answering");
  } else {
    _up.fetch_sub(1, std::memory_order_relaxed);
    _logger->warn("rtpengine at " + engine.host + ":" + std::to_string(engine.port) + " is not answering: no new calls go to it");
  }
}

std::string RtpengineMediaEngine::_advertise_for(const Flags& flags) const { return flags.address.empty() ? _public.current() : flags.address; }

void RtpengineMediaEngine::close() {
  _up.store(0, std::memory_order_relaxed);
  _public.stop();

  std::shared_ptr<boost::asio::ip::udp::socket> socket;
  std::shared_ptr<boost::asio::ip::udp::socket> events;

  {
    std::lock_guard<std::mutex> lock(_socket_mutex);
    socket = std::move(_socket);
    events = std::move(_events_socket);
  }

  if (!socket) return;

  // The socket is closed on the strand, which ends the receive loop with operation_aborted and drops the loop's
  // reference to this object. Pending requests are failed so no caller waits for ever; the object is held weakly
  // because close() also runs from the destructor.
  auto weak = weak_from_this();

  boost::asio::post(_strand, [socket, events, weak]() {
    boost::system::error_code ec;
    socket->close(ec);
    if (events) events->close(ec);

    if (auto self = weak.lock()) {
      if (self->_ping_timer) self->_ping_timer->cancel();
      for (auto& engine : self->_engines) engine.up = false;
      self->_up.store(0, std::memory_order_relaxed);
      self->_fail_all("the media engine was closed");
    }
  });

  _logger->info("Closed");
}

void RtpengineMediaEngine::_fail_all(const std::string& reason) {
  auto pending = std::move(_pending);
  _pending.clear();

  for (auto& [cookie, entry] : pending) {
    if (entry->timer) entry->timer->cancel();
    if (entry->handler) entry->handler(std::nullopt);
  }

  if (!pending.empty()) _logger->warn(std::to_string(pending.size()) + " request(s) abandoned: " + reason);
}

Bencode RtpengineMediaEngine::_command(const std::string& name, const std::shared_ptr<Call>& call) const {
  auto command = Bencode::dictionary({{"command", Bencode(name)}});
  if (call) command.set("call-id", Bencode(call->id));
  return command;
}

std::shared_ptr<boost::asio::ip::udp::socket> RtpengineMediaEngine::_current_socket() const {
  std::lock_guard<std::mutex> lock(_socket_mutex);
  return _socket;
}

void RtpengineMediaEngine::_request(Bencode command, std::size_t engine, Reply reply) {
  auto self = shared_from_this();

  boost::asio::dispatch(_strand, [this, self, command = std::move(command), engine, reply = std::move(reply)]() mutable {
    auto socket = _current_socket();

    if (!socket || !socket->is_open() || engine >= _engines.size() || !_engines[engine].resolved) {
      if (reply) reply(std::nullopt);
      return;
    }

    auto pending = std::make_shared<Pending>();
    pending->cookie = Util::generate_random_string("", 16);
    pending->datagram = pending->cookie + " " + command.encode();
    pending->engine = engine;
    pending->ping = command.string_at("command") == "ping";
    pending->handler = std::move(reply);
    pending->attempts_left = _attempts;
    pending->timer = std::make_shared<boost::asio::steady_timer>(_strand);

    _pending[pending->cookie] = pending;
    _send(pending);
  });
}

std::optional<std::size_t> RtpengineMediaEngine::_engine_named(const std::string& name) const {
  for (std::size_t i = 0; i < _engines.size(); ++i) {
    if (_engines[i].name() == name) return i;
  }
  return std::nullopt;
}

std::optional<std::size_t> RtpengineMediaEngine::_choose(const std::string& call_id, const std::vector<bool>& tried) const {
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < _engines.size(); ++i) {
    if (_engines[i].up && !tried[i]) candidates.push_back(i);
  }

  if (candidates.empty()) return std::nullopt;
  return candidates[hash_of(call_id) % candidates.size()];
}

void RtpengineMediaEngine::_call_request(Bencode command, std::string recorded, std::string call_id, CallReply reply) {
  auto self = shared_from_this();

  boost::asio::dispatch(
      _strand, [this, self, command = std::move(command), recorded = std::move(recorded), call_id = std::move(call_id), reply = std::move(reply)]() mutable {
        if (recorded.empty()) {
          return _try_engines(std::make_shared<Bencode>(std::move(command)), std::move(call_id), std::make_shared<std::vector<bool>>(_engines.size(), false),
                              std::move(reply));
        }

        // The call's ports are on that engine and nowhere else, answering or not.
        const auto engine = _engine_named(recorded);
        if (!engine) return reply(Answered{std::nullopt, recorded, "the call is on " + recorded + ", which is not in this node's rtpengine pool"});

        _request(std::move(command), *engine,
                 [recorded, reply = std::move(reply)](std::optional<Bencode> answer) { reply(Answered{std::move(answer), recorded, {}}); });
      });
}

void RtpengineMediaEngine::_try_engines(std::shared_ptr<Bencode> command, std::string call_id, std::shared_ptr<std::vector<bool>> tried, CallReply reply) {
  const auto engine = _choose(call_id, *tried);
  if (!engine) return reply(Answered{std::nullopt, {}, "no rtpengine is answering"});

  (*tried)[*engine] = true;

  auto self = shared_from_this();
  const auto name = _engines[*engine].name();

  _request(*command, *engine, [this, self, command, call_id, tried, name, reply = std::move(reply)](std::optional<Bencode> answer) mutable {
    if (!answer) return _try_engines(std::move(command), std::move(call_id), std::move(tried), std::move(reply));
    reply(Answered{std::move(answer), name, {}});
  });
}

void RtpengineMediaEngine::_send(const std::shared_ptr<Pending>& pending) {
  auto socket = _current_socket();
  if (!socket || !socket->is_open() || pending->attempts_left == 0) return;

  --pending->attempts_left;

  // Synchronous: a datagram send does not block, and this is already on the strand.
  boost::system::error_code ec;
  socket->send_to(boost::asio::buffer(pending->datagram), _engines[pending->engine].endpoint, 0, ec);

  if (ec) {
    _logger->warn("Could not send to rtpengine - " + ec.message());

    // A send that failed outright would fail again, so report now rather than after the retransmissions.
    auto handler = std::move(pending->handler);
    _pending.erase(pending->cookie);
    _mark(pending->engine, false);
    if (handler) handler(std::nullopt);
    return;
  }

  auto self = shared_from_this();
  const auto cookie = pending->cookie;

  pending->timer->expires_after(std::chrono::milliseconds(_timeout_ms));
  pending->timer->async_wait(boost::asio::bind_executor(_strand, [this, self, cookie](const boost::system::error_code& error) {
    if (error) return;
    _on_timeout(cookie);
  }));
}

void RtpengineMediaEngine::_on_timeout(const std::string& cookie) {
  auto search = _pending.find(cookie);
  if (search == _pending.end()) return;

  auto pending = search->second;

  if (pending->attempts_left > 0) {
    _logger->warn("No answer from " + _engines[pending->engine].name() + " in " + std::to_string(_timeout_ms) + "ms, asking again");
    return _send(pending);
  }

  auto handler = std::move(pending->handler);
  _pending.erase(search);
  _mark(pending->engine, false);

  if (handler) handler(std::nullopt);
}

// Each continuation holds self, keeping the buffer and this object alive. close() ends the loop: the receive
// completes with operation_aborted and the reference goes.
void RtpengineMediaEngine::_receive() {
  auto socket = _current_socket();
  if (!socket || !socket->is_open()) return;

  auto self = shared_from_this();

  socket->async_receive_from(
      boost::asio::buffer(_receive_buffer), _received_from,
      boost::asio::bind_executor(_strand, [this, self, socket](const boost::system::error_code& ec, std::size_t length) { _on_receive(ec, length); }));
}

bool RtpengineMediaEngine::dtmf_listen(DtmfListener listener) {
  _dtmf_listener = std::move(listener);
  return !_dtmf_events.empty();
}

void RtpengineMediaEngine::_events_receive() {
  std::shared_ptr<boost::asio::ip::udp::socket> socket;
  {
    std::lock_guard<std::mutex> lock(_socket_mutex);
    socket = _events_socket;
  }
  if (!socket || !socket->is_open()) return;

  if (_event_buffer.size() < 4096) _event_buffer.resize(4096);

  auto self = shared_from_this();
  socket->async_receive_from(
      boost::asio::buffer(_event_buffer), _event_from,
      boost::asio::bind_executor(_strand, [this, self, socket](const boost::system::error_code& ec, std::size_t length) { _on_event(ec, length); }));
}

// One datagram per key press, as daemon/dtmf.c writes it: {"callid", "source_tag", "tags", "type": "DTMF",
// "timestamp", "source_ip", "event", "duration", "volume"}; event is 0 to 15 (0-9, *, #, A-D), duration in ms.
void RtpengineMediaEngine::_on_event(const boost::system::error_code& ec, std::size_t length) {
  if (ec) {
    if (ec != boost::asio::error::operation_aborted) {
      _logger->warn("Receiving a key press failed - " + ec.message());
      _events_receive();
    }
    return;
  }

  const std::string datagram(_event_buffer.data(), length);
  boost::system::error_code parse_error;
  const auto parsed = boost::json::parse(datagram, parse_error);

  if (parse_error || !parsed.is_object()) {
    _logger->warn("A key press from rtpengine was not JSON - ignored");
  } else {
    const auto& object = parsed.as_object();
    const auto* call_id = object.if_contains("callid");
    const auto* source_tag = object.if_contains("source_tag");
    const auto* event = object.if_contains("event");
    const auto* duration = object.if_contains("duration");

    static constexpr char kDigits[] = "0123456789*#ABCD";
    const auto code = event != nullptr && event->is_number() ? event->to_number<std::int64_t>() : -1;

    if (call_id == nullptr || !call_id->is_string() || source_tag == nullptr || !source_tag->is_string() || code < 0 || code > 15) {
      _logger->warn("A key press from rtpengine named no call, tag or digit - ignored");
    } else if (_dtmf_listener) {
      DtmfEvent pressed;
      pressed.call_id = std::string(call_id->as_string());
      pressed.source_tag = std::string(source_tag->as_string());
      pressed.digit = kDigits[code];
      pressed.duration = std::chrono::milliseconds(duration != nullptr && duration->is_number() ? duration->to_number<std::int64_t>() : 0);
      _dtmf_listener(std::move(pressed));
    }
  }

  _events_receive();
}

void RtpengineMediaEngine::_on_receive(const boost::system::error_code& ec, std::size_t length) {
  if (ec) {
    // operation_aborted is close(); after anything else, carry on, since the next datagram may be fine.
    if (ec != boost::asio::error::operation_aborted) {
      _logger->warn("Receive failed - " + ec.message());
      _receive();
    }
    return;
  }

  const std::string datagram(_receive_buffer.data(), length);
  const auto space = datagram.find(' ');

  if (space == std::string::npos) {
    _logger->warn("Datagram with no cookie, ignored");
    _receive();
    return;
  }

  const auto cookie = datagram.substr(0, space);
  auto search = _pending.find(cookie);

  if (search == _pending.end()) {
    // A reply to a request already given up on, or never sent.
    _receive();
    return;
  }

  auto pending = search->second;

  // Review finding 33: the cookie names the request, but only the engine it went to may answer it. A datagram from
  // anywhere else could carry forged SDP and media addresses; it is dropped, and the request waits for the engine.
  if (pending->engine < _engines.size()) {
    const auto& expected = _engines[pending->engine].endpoint;
    if (_received_from.address() != expected.address() || _received_from.port() != expected.port()) {
      _logger->warn("A reply for " + cookie + " from " + _received_from.address().to_string() + ":" + std::to_string(_received_from.port()) +
                    ", not the engine at " + expected.address().to_string() + ":" + std::to_string(expected.port()) + " - dropped");
      _receive();
      return;
    }
  }

  _pending.erase(search);

  if (pending->timer) pending->timer->cancel();

  // Any answer at all shows the engine is there; a ping's is judged by the ping.
  if (!pending->ping) _mark(pending->engine, true);

  auto reply = Bencode::decode(std::string_view(datagram).substr(space + 1));

  if (!reply) _logger->warn("Could not decode the answer from rtpengine");

  if (pending->handler) pending->handler(std::move(reply));

  _receive();
}

std::string RtpengineMediaEngine::_error_of(const std::optional<Bencode>& reply) {
  if (!reply) return "no answer from rtpengine";

  const auto result = reply->string_at("result");
  if (result == "ok" || result == "pong") return "";

  const auto reason = reply->string_at("error-reason");
  if (!reason.empty()) return reason;

  return result.empty() ? "rtpengine answered without a result" : "rtpengine answered \"" + result + "\"";
}

// RFC 3261 12.1.1. rtpengine wants the offerer as from-tag throughout an exchange, so an answer names the offerer
// as from and the answerer as to.
RtpengineMediaEngine::Tags RtpengineMediaEngine::_tags_for(const std::shared_ptr<Call>& call, const Flags& flags, bool answering) {
  Tags tags;

  if (!call || flags.participant >= call->participants.size()) return tags;

  const auto& participant = call->participants[flags.participant];
  const auto& dialog = participant.dialog;

  if (!dialog) return tags;

  const auto& own = participant.originator ? dialog->caller_tag : dialog->callee_tag;
  const auto& other = participant.originator ? dialog->callee_tag : dialog->caller_tag;

  if (answering) {
    // The offerer is the other end.
    if (other.empty() || own.empty()) return tags;
    tags.from = other;
    tags.to = own;
  } else {
    if (own.empty()) return tags;
    tags.from = own;
    tags.to = other;
  }

  tags.ok = true;
  return tags;
}

namespace {

// What the call records, when it is an engine of this driver's; anything else (an older record, another driver's)
// leaves the choice open.
std::string recorded_engine(const std::shared_ptr<Call>& call) {
  if (!call || call->media_engine.rfind("rtpengine://", 0) != 0) return "";
  return call->media_engine;
}

// Completes an offer or answer, recording the engine on the call on the caller's executor, where the call is read.
void complete_media(plugins::Executor on, std::shared_ptr<Call> call, std::string engine, MediaEngine::MediaHandler handler, Result result) {
  MediaEngine::MediaHandler record = [call, engine = std::move(engine), handler = std::move(handler)](Result result) mutable {
    if (result.ok && call && !engine.empty()) call->media_engine = engine;
    if (handler) handler(std::move(result));
  };

  boost::asio::post(std::move(on), [record = std::move(record), result = std::move(result)]() mutable { record(std::move(result)); });
}

}  // namespace

void RtpengineMediaEngine::offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  if (!is_connected()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine is not connected"));

  const auto tags = _tags_for(call, flags, false);
  if (!tags.ok) return _complete(std::move(on), std::move(handler), Result::failure("no dialog tag for the offering participant"));

  auto command = _command("offer", call);
  command.set("from-tag", Bencode(tags.from));
  if (!tags.to.empty()) command.set("to-tag", Bencode(tags.to));
  command.set("sdp", Bencode(std::move(sdp)));

  // o= and the session-level c= name this node, so an anchored call does not give one end the other's address
  // (RFC 8866 section 5.2). sdp-version has rtpengine version what it emits (RFC 3264 section 8).
  command.set("replace", Bencode::list({Bencode(std::string("origin")), Bencode(std::string("session-connection")), Bencode(std::string("sdp-version"))}));

  apply_profile(command, flags.target, flags.rtcp_mux, false);
  apply_dtmf(command, flags);

  // The far leg speaks INFO: the key presses this leg sends are reported here, to be sent on as INFO.
  if (flags.dtmf_to == types::MediaPolicy::Dtmf::Info && !_events_destination.empty()) command.set("DTMF-log-dest", Bencode(_events_destination));

  if (const auto advertise = _advertise_for(flags); !advertise.empty()) command.set("media-address", Bencode(advertise));

  _call_request(std::move(command), recorded_engine(call), call->id, [call, on, handler = std::move(handler)](Answered answered) mutable {
    const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
    if (!error.empty()) return complete_media(std::move(on), call, {}, std::move(handler), Result::failure(error));

    auto sdp = answered.reply->string_at("sdp");
    if (sdp.empty()) return complete_media(std::move(on), call, {}, std::move(handler), Result::failure("rtpengine answered ok with no sdp"));

    complete_media(std::move(on), call, answered.engine, std::move(handler), Result::success(std::move(sdp)));
  });
}

void RtpengineMediaEngine::answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  if (!is_connected()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine is not connected"));

  const auto tags = _tags_for(call, flags, true);
  if (!tags.ok) return _complete(std::move(on), std::move(handler), Result::failure("no dialog tags for the answering participant"));

  auto command = _command("answer", call);
  command.set("from-tag", Bencode(tags.from));
  command.set("to-tag", Bencode(tags.to));
  command.set("sdp", Bencode(std::move(sdp)));
  command.set("replace", Bencode::list({Bencode(std::string("origin")), Bencode(std::string("session-connection")), Bencode(std::string("sdp-version"))}));

  apply_profile(command, flags.target, flags.rtcp_mux, true);
  apply_dtmf(command, flags);
  if (const auto advertise = _advertise_for(flags); !advertise.empty()) command.set("media-address", Bencode(advertise));

  _call_request(std::move(command), recorded_engine(call), call->id, [call, on, handler = std::move(handler)](Answered answered) mutable {
    const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
    if (!error.empty()) return complete_media(std::move(on), call, {}, std::move(handler), Result::failure(error));

    auto sdp = answered.reply->string_at("sdp");
    if (sdp.empty()) return complete_media(std::move(on), call, {}, std::move(handler), Result::failure("rtpengine answered ok with no sdp"));

    complete_media(std::move(on), call, answered.engine, std::move(handler), Result::success(std::move(sdp)));
  });
}

void RtpengineMediaEngine::release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::success());
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  // No tags: a delete naming only the call releases every leg of it.
  _call_request(_command("delete", call), recorded_engine(call), call->id, [on, handler = std::move(handler)](Answered answered) mutable {
    const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
    _complete(std::move(on), std::move(handler), error.empty() ? plugins::Status::success() : plugins::Status::failure(error));
  });
}

void RtpengineMediaEngine::query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success("{}"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure("rtpengine is not connected"));

  auto self = shared_from_this();

  _call_request(_command("query", call), recorded_engine(call), call->id, [this, self, call, on, handler = std::move(handler)](Answered answered) mutable {
    if (!answered.error.empty()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure(answered.error));

    // The engine holds nothing for the call: said as such, so a node can tell media that has gone from media it cannot see.
    if (answered.reply && answered.reply->string_at("result") == "error" &&
        Util::to_lower(answered.reply->string_at("error-reason")).find("unknown call") != std::string::npos) {
      return _complete(std::move(on), std::move(handler),
                       plugins::Result<std::string>::success("{\"call_id\":\"" + call->id +
                                                             "\",\"engine\":\"rtpengine\",\"held\":false,\"streams\":0,\"idle_seconds\":null,\"legs\":[]}"));
    }

    const auto error = _error_of(answered.reply);
    if (!error.empty()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure(error));

    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(_idle_document(call, *answered.reply)));
  });
}

// The query document, in the builtin engine's shape. idle_seconds is the time since the most recent packet on any
// stream: one stream still carrying is a call still up. rtpengine reports "last packet" per stream as a unix
// timestamp, zero if nothing has arrived; such a stream counts as idle since the call was created.
std::string RtpengineMediaEngine::_idle_document(const std::shared_ptr<Call>& call, const Bencode& reply) const {
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const auto created = reply.integer_at("created", now);

  std::int64_t newest = 0;
  std::size_t streams = 0;

  // Per stream: "stats" is what arrived from the stream's end and "stats_out", where rtpengine reports it, what was sent to it.
  std::string legs = "[";

  if (const auto* tags = reply.find("tags"); tags != nullptr && tags->is_dictionary()) {
    for (const auto& [tag, leg] : tags->entries()) {
      const auto* medias = leg.find("medias");
      if (medias == nullptr || !medias->is_list()) continue;

      for (const auto& media : medias->values()) {
        const auto* leg_streams = media.find("streams");
        if (leg_streams == nullptr || !leg_streams->is_list()) continue;

        for (const auto& stream : leg_streams->values()) {
          ++streams;

          const auto last = stream.integer_at("last packet", 0);
          const auto seen = last > 0 ? last : created;

          if (seen > newest) newest = seen;

          if (legs.size() > 1) legs += ",";
          legs += "{";
          const auto* in = stream.find("stats");
          legs += "\"packets_in\":" + std::to_string(in && in->is_dictionary() ? in->integer_at("packets", 0) : 0);
          legs += ",\"bytes_in\":" + std::to_string(in && in->is_dictionary() ? in->integer_at("bytes", 0) : 0);
          if (const auto* out = stream.find("stats_out"); out && out->is_dictionary()) {
            legs += ",\"packets_out\":" + std::to_string(out->integer_at("packets", 0));
            legs += ",\"bytes_out\":" + std::to_string(out->integer_at("bytes", 0));
          }
          legs += "}";
        }
      }
    }
  }

  // A call with no streams reports null, not a number the call sweep would act on.
  const auto idle = streams == 0 ? std::string("null") : std::to_string(now > newest ? now - newest : 0);

  legs += "]";

  return "{\"call_id\":\"" + call->id + "\",\"engine\":\"rtpengine\",\"streams\":" + std::to_string(streams) + ",\"idle_seconds\":" + idle +
         ",\"legs\":" + legs + "}";
}

// One "play DTMF" per digit, in order; rtpengine queues them. The participant named is the one the event comes
// from, so the call must have been offered and answered with inject DTMF (apply_dtmf), which is what a leg set to
// info earns.
void RtpengineMediaEngine::play_dtmf(plugins::Executor on, std::shared_ptr<Call> call, std::size_t participant, std::string digits,
                                     std::chrono::milliseconds duration, plugins::StatusHandler handler) {
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));
  if (digits.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

  Flags flags;
  flags.participant = participant;
  const auto tags = _tags_for(call, flags, false);
  if (!tags.ok) return _complete(std::move(on), std::move(handler), plugins::Status::failure("no dialog tag for the participant"));

  auto command = _command("play DTMF", call);
  command.set("from-tag", Bencode(tags.from));
  command.set("code", Bencode(std::string(1, digits.front())));
  command.set("duration", Bencode(static_cast<std::int64_t>(std::clamp<std::int64_t>(duration.count(), 100, 5000))));

  auto self = shared_from_this();
  _call_request(std::move(command), recorded_engine(call), call->id,
                [this, self, on, call, participant, rest = digits.substr(1), duration, handler = std::move(handler)](Answered answered) mutable {
                  const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
                  if (!error.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::failure(error));
                  play_dtmf(std::move(on), call, participant, rest, duration, std::move(handler));
                });
}

void RtpengineMediaEngine::start_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::failure("no call"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  _call_request(_command("start recording", call), recorded_engine(call), call->id, [on, handler = std::move(handler)](Answered answered) mutable {
    const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
    _complete(std::move(on), std::move(handler), error.empty() ? plugins::Status::success() : plugins::Status::failure(error));
  });
}

void RtpengineMediaEngine::stop_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::failure("no call"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  _call_request(_command("stop recording", call), recorded_engine(call), call->id, [on, handler = std::move(handler)](Answered answered) mutable {
    const auto error = answered.error.empty() ? _error_of(answered.reply) : answered.error;
    _complete(std::move(on), std::move(handler), error.empty() ? plugins::Status::success() : plugins::Status::failure(error));
  });
}

}  // namespace athenasip::media
