//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "builtin_media_engine.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <utility>

#include "../config.h"
#include "../global_io_context.h"
#include "../sdp.h"

namespace athenasip::media {

namespace {

std::unordered_map<std::string, std::string> parse_query(const std::string& query) {
  std::unordered_map<std::string, std::string> result;
  std::istringstream stream(query);
  std::string pair;

  while (std::getline(stream, pair, '&')) {
    const auto equals = pair.find('=');
    if (equals == std::string::npos) continue;
    result[pair.substr(0, equals)] = pair.substr(equals + 1);
  }

  return result;
}

std::uint16_t parse_port(const std::string& text, std::uint16_t fallback) {
  if (text.empty() || text.size() > 5) return fallback;

  unsigned long value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return fallback;
    value = (value * 10) + static_cast<unsigned long>(c - '0');
  }

  if (value == 0 || value > 65535) return fallback;
  return static_cast<std::uint16_t>(value);
}

}  // namespace

BuiltinMediaEngine::BuiltinMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("builtin_media", std::move(logger))), _url(std::move(url)) {
  _apply_url(_url);
}

BuiltinMediaEngine::~BuiltinMediaEngine() { close(); }

std::string BuiltinMediaEngine::name() const { return "builtin"; }

std::string BuiltinMediaEngine::version() const { return "0.0.1"; }

void BuiltinMediaEngine::_apply_url(const std::shared_ptr<types::URL>& url) {
  if (!url) return;

  const auto params = parse_query(url->query);

  auto it = params.find("bind_address");
  if (it != params.end() && !it->second.empty()) _bind_address = it->second;

  it = params.find("public_address");
  if (it != params.end() && !it->second.empty()) _public_address = it->second;

  it = params.find("port_min");
  if (it != params.end()) _port_min = parse_port(it->second, _port_min);

  it = params.find("port_max");
  if (it != params.end()) _port_max = parse_port(it->second, _port_max);

  _validate_port_range();
}

bool BuiltinMediaEngine::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (!own_root || !own_root.IsMap()) {
    return true;
  }

  if (own_root["bind_address"]) _bind_address = own_root["bind_address"].as<std::string>();
  if (own_root["public_address"]) _public_address = own_root["public_address"].as<std::string>();
  if (own_root["port_min"]) _port_min = parse_port(own_root["port_min"].as<std::string>(), _port_min);
  if (own_root["port_max"]) _port_max = parse_port(own_root["port_max"].as<std::string>(), _port_max);

  _validate_port_range();
  return true;
}

void BuiltinMediaEngine::_validate_port_range() {
  if (_port_max <= _port_min) {
    _logger->warn("port_max is not above port_min, using the defaults");
    _port_min = 22000;
    _port_max = 23000;
  }
}

void BuiltinMediaEngine::connect(plugins::Executor on, plugins::StatusHandler handler) {
  if (_connected) return _complete(std::move(on), std::move(handler), plugins::Status::success());

  _relay = std::make_shared<rtp::RTPRelay>(_logger, _bind_address, _port_min, _port_max);
  _relay->start();
  _connected = true;

  // A name is looked up now, before the node serves anything, so the first call has an
  // address; and then kept up to date in the background.
  boost::system::error_code literal;
  boost::asio::ip::make_address(_public_address, literal);
  if (literal) {
    {
      std::lock_guard<std::mutex> lock(_public->mutex);
      _public->name = _public_address;
    }

    boost::asio::io_context io;
    boost::asio::ip::udp::resolver resolver(io);
    boost::system::error_code failed;
    const auto found = resolver.resolve(boost::asio::ip::udp::v4(), _public_address, "", failed);

    if (!failed && !found.empty()) {
      std::lock_guard<std::mutex> lock(_public->mutex);
      _public->address = found.begin()->endpoint().address().to_string();
      _logger->info("Public address " + _public_address + " is " + _public->address);
    } else {
      _logger->error("Public address " + _public_address + " does not resolve - media will not be anchored until it does");
    }

    _public_refresh_schedule();
  }

  _logger->info("Connected, relaying on " + _bind_address + " as " + _public_address);
  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

std::string BuiltinMediaEngine::_public_for_media() const {
  std::lock_guard<std::mutex> lock(_public->mutex);
  return _public->name.empty() ? _public_address : _public->address;
}

// Every minute, on the process's own io_context and never on the Core strand: a lookup
// is a network round trip. A change is logged, because it is the site's address moving.
void BuiltinMediaEngine::_public_refresh_schedule() {
  auto timer = std::make_shared<boost::asio::steady_timer>(detail::get_global_io_context());
  _public_refresh = timer;

  std::weak_ptr<PublicName> weak_public = _public;
  std::weak_ptr<boost::asio::steady_timer> weak_timer = timer;
  auto logger = _logger;

  // The engine owns the loop; each step holds it weakly, so closing the engine ends it.
  auto tick = std::make_shared<std::function<void()>>();
  _public_tick = tick;
  std::weak_ptr<std::function<void()>> weak_tick = tick;

  *tick = [weak_public, weak_timer, logger, weak_tick]() {
    auto timer = weak_timer.lock();
    if (!timer) return;

    timer->expires_after(std::chrono::seconds(60));
    timer->async_wait([weak_public, weak_timer, logger, weak_tick](boost::system::error_code ec) {
      if (ec) return;
      auto state = weak_public.lock();
      if (!state) return;

      std::string name;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        name = state->name;
      }

      auto resolver = std::make_shared<boost::asio::ip::udp::resolver>(detail::get_global_io_context());
      resolver->async_resolve(
          boost::asio::ip::udp::v4(), name, "",
          [resolver, weak_public, logger, name, weak_tick](boost::system::error_code failed, boost::asio::ip::udp::resolver::results_type found) {
            if (auto state = weak_public.lock(); state && !failed && !found.empty()) {
              const auto address = found.begin()->endpoint().address().to_string();
              std::lock_guard<std::mutex> lock(state->mutex);
              if (address != state->address) {
                logger->info("Public address " + name + " is now " + address + (state->address.empty() ? "" : ", was " + state->address));
                state->address = address;
              }
            }
            if (auto next = weak_tick.lock()) (*next)();
          });
    });
  };

  (*tick)();
}

void BuiltinMediaEngine::close() {
  if (!_connected) return;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _allocated.clear();
    _emitted.clear();
  }

  if (_public_refresh) _public_refresh->cancel();
  _public_refresh.reset();
  _public_tick.reset();

  if (_relay) _relay->stop();
  _relay.reset();
  _connected = false;

  _logger->info("Closed");
}

bool BuiltinMediaEngine::is_connected() const { return _connected; }

Capabilities BuiltinMediaEngine::capabilities() const {
  Capabilities capabilities;
  capabilities.bridge = true;
  return capabilities;
}

// A relay and nothing more: what comes in plain goes out plain. Mirror is whatever the
// caller sent, which is what this engine sends on.
bool BuiltinMediaEngine::produces(Profile profile) const { return profile == Profile::PlainRtp || profile == Profile::Mirror; }

// The relay is in this process and answers at once; the contract is about where the
// handler runs. Posting it is what lets an rtpengine driver, which really does go to
// the network, be dropped in without the caller changing.
void BuiltinMediaEngine::offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  _complete(std::move(on), std::move(handler), _offer(std::move(call), sdp, flags));
}

void BuiltinMediaEngine::answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  _complete(std::move(on), std::move(handler), _answer(std::move(call), sdp, flags));
}

void BuiltinMediaEngine::release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_release(std::move(call)), "release"));
}

void BuiltinMediaEngine::query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(_query(std::move(call))));
}

Result BuiltinMediaEngine::_offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) {
  // Plain RTP only. An offer needing ICE, DTLS or SRTP belongs to rtpengine.
  if (flags.ice || flags.dtls || flags.srtp) {
    return Result::failure("builtin media engine handles plain RTP only: use rtpengine:// for ICE, DTLS or SRTP");
  }

  return _map_media(std::move(call), sdp, flags);
}

Result BuiltinMediaEngine::_answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) {
  if (flags.ice || flags.dtls || flags.srtp) {
    return Result::failure("builtin media engine handles plain RTP only: use rtpengine:// for ICE, DTLS or SRTP");
  }

  return _map_media(std::move(call), sdp, flags);
}

Result BuiltinMediaEngine::_map_media(std::shared_ptr<Call> call, const std::string& sdp_text, const Flags& flags) {
  if (!_connected || !_relay) return Result::failure("media engine is not connected");
  if (!call) return Result::failure("no call");
  if (flags.participant >= call->participants.size()) return Result::failure("no such participant");

  auto sdp = std::make_shared<SDP>();
  if (!sdp->parse(sdp_text)) return Result::failure("could not parse SDP");

  // Where this leg said to send its media, read before it is rewritten to be the relay's.
  const auto session_address = sdp->has_connection() ? sdp->connection().address : std::string();

  // Make us the endpoint for everything.
  ConnectionInfo relay;
  relay.nettype = "IN";
  relay.addrtype = "IP4";
  relay.address = flags.address.empty() ? _public_for_media() : flags.address;

  // A name that resolves to nothing would send the far end's media nowhere if it were
  // written; declined, the description travels on as it came.
  if (relay.address.empty()) return Result::failure("the public address " + _public_address + " does not resolve to an address");

  sdp->set_connection(relay);

  // RFC 8866 section 5.2: the o= line gives "an address of the machine from which the
  // session was created", and a node that anchors media so the two ends never see each
  // other's addresses hands one of them away in it regardless. The RFC allows exactly
  // this substitution - "for privacy reasons, it is sometimes desirable to obfuscate the
  // username and IP address of the session originator" - on the condition that the field
  // stays globally unique. The username and session id the endpoint chose are what carry
  // that uniqueness and are left alone; only the address is replaced. The version is the
  // endpoint's too, and 3264's rule for incrementing it is its own item in the plan.
  auto origin = sdp->origin();
  origin.nettype = relay.nettype;
  origin.addrtype = relay.addrtype;
  origin.address = relay.address;
  sdp->set_origin(origin);

  auto& participant = call->participants[flags.participant];

  for (auto& media : sdp->media()) {
    const auto id = media.unique_id();

    // RFC 3264 sections 5.1, 6 and 8.2: a stream with port zero is one that is not offered,
    // was declined, or has been taken away. It keeps its place in the description and gets
    // nothing else: no relay, and above all no relay port, which would tell the other end
    // that a stream its peer refused had been accepted. A phone with no camera answering a
    // video call is the usual one. What the stream held, if anything, goes back.
    if (media.description.port == 0) {
      std::lock_guard<std::mutex> lock(_mutex);
      auto& streams = _allocated[call->id];

      if (auto declined = streams.find(id); declined != streams.end()) {
        _relay->release_relay_set(declined->second.rtp);
        _relay->release_relay_set(declined->second.rtcp);
        streams.erase(declined);
      }

      participant.streams.erase(id);
      continue;
    }

    StreamRelays relays;

    {
      std::lock_guard<std::mutex> lock(_mutex);
      auto& streams = _allocated[call->id];
      auto existing = streams.find(id);

      // The stream is the call's, not the leg's. The second leg to arrive for a stream
      // is the far end of a bridge already standing, and giving it a port of its own
      // would leave each end talking to a relay nothing else is on.
      if (existing != streams.end()) {
        relays = existing->second;
        _logger->debug("Mapping media, existing stream: " + media.description.to_string());
      } else {
        relays.rtp = _relay->allocate_relay_set();
        relays.rtcp = _relay->allocate_relay_set();

        if (!relays.rtp || !relays.rtcp) {
          if (relays.rtp) _relay->release_relay_set(relays.rtp);
          if (relays.rtcp) _relay->release_relay_set(relays.rtcp);
          return Result::failure("no relay ports available");
        }

        relays.rtp->start();
        relays.rtcp->start();

        streams.emplace(id, relays);
        _logger->debug("Mapping media, created stream: " + media.description.to_string());
      }
    }

    // What the leg itself holds, for everything that reads a call rather than relays it.
    auto& stream = participant.streams[id];
    if (!stream) {
      stream = std::make_shared<MediaStream>();
      stream->id = std::to_string(id);
    }

    stream->rtp_set = relays.rtp;
    stream->rtcp_set = relays.rtcp;

    // The relay starts sending to this leg where its description said, so a leg that only
    // listens gets media before it has sent any. RTCP is where a=rtcp says (RFC 3605), or
    // the port above (RFC 3550 11).
    const auto described_address = media.has_connection() ? media.connection().address : session_address;
    const auto described_port = media.description.port;
    if (!described_address.empty() && described_port != 0) {
      relays.rtp->expect(described_address, static_cast<std::uint16_t>(described_port));

      auto rtcp_port = static_cast<std::uint16_t>(described_port + 1);
      for (const auto& attribute : media.attributes()) {
        if (attribute.rfind("rtcp:", 0) != 0) continue;
        try {
          rtcp_port = static_cast<std::uint16_t>(std::stoul(attribute.substr(5)));
        } catch (const std::exception&) {
        }
      }
      relays.rtcp->expect(described_address, rtcp_port);
    }

    // Point the media at our relay port.
    media.description.port = relays.rtp->port;

    // Only rewrite a media-level c= that was already there. Adding one where the far
    // end relied on the session-level line would change the shape of the offer.
    if (media.has_connection()) media.set_connection(relay);

    // RFC 3605, and written whether or not the far end named a port of its own. The
    // relay's RTCP port comes out of the same pool as its RTP port and is not reliably
    // the one above it, so an endpoint left to assume the convention would send its
    // receiver reports into somebody else's call.
    const auto rtcp = "rtcp:" + std::to_string(relays.rtcp->port) + " IN IP4 " + relay.address;
    if (!media.set_attribute("rtcp:", rtcp)) media.add_attribute(rtcp);
  }

  _apply_version(call, flags, *sdp);

  return Result::success(sdp->to_string());
}

// RFC 3264 section 8: an offer that changes the description must carry a higher version
// than the one before it, and one that does not must carry the same. The endpoint's own
// version cannot be passed through to answer that, because what this node emits is not
// what the endpoint sent: the addresses and the ports are this node's, and two offers
// an endpoint considered identical can come out of here different - a stream released
// and re-anchored gets other ports.
//
// So the version is this node's from the second description onwards. The first keeps
// the endpoint's, which is what makes a call that never re-offers look exactly as it
// did before.
void BuiltinMediaEngine::_apply_version(const std::shared_ptr<Call>& call, const Flags& flags, SDP& sdp) {
  auto origin = sdp.origin();

  // The shape is the body with the version taken out, so that comparing two of them
  // asks whether anything else changed.
  auto blanked = origin;
  blanked.sessionVersion = "0";
  sdp.set_origin(blanked);

  const auto shape = sdp.to_string();

  std::uint64_t version = 0;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    auto& last = _emitted[call->id][flags.participant];

    if (last.shape.empty()) {
      // Nothing emitted yet, so the endpoint's own version is as good a starting point
      // as any and leaves an unchanging call looking untouched.
      version = 0;
      try {
        version = std::stoull(origin.sessionVersion);
      } catch (const std::exception&) {
        version = 0;
      }
    } else {
      version = shape == last.shape ? last.version : last.version + 1;
    }

    last.shape = shape;
    last.version = version;
  }

  origin.sessionVersion = std::to_string(version);
  sdp.set_origin(origin);
}

bool BuiltinMediaEngine::_release(std::shared_ptr<Call> call) {
  if (!call) return false;

  std::unordered_map<std::int64_t, StreamRelays> held;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _allocated.find(call->id);

    // Releasing a call the engine never saw is not an error.
    if (it == _allocated.end()) return true;

    held = std::move(it->second);
    _allocated.erase(it);
    _emitted.erase(call->id);

    if (_relay) {
      for (const auto& [id, relays] : held) {
        _relay->release_relay_set(relays.rtp);
        _relay->release_relay_set(relays.rtcp);
      }
    }
  }

  for (auto& participant : call->participants) participant.streams.clear();

  _logger->debug("Released " + std::to_string(held.size() * 2) + " relay sets for call " + call->id);
  return true;
}

std::string BuiltinMediaEngine::_query(std::shared_ptr<Call> call) {
  if (!call) return "{}";

  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _allocated.find(call->id);
  const auto count = (it == _allocated.end()) ? 0u : it->second.size() * 2;

  // How long every relay this call holds has been silent, which is the shortest idle of
  // any of them: one stream still carrying is a call still up. RTP and RTCP both count,
  // so a call on hold or one whose codec suppresses silence does not read as dead.
  //
  // A call the engine holds nothing for has no media to be idle, and says so with -1
  // rather than with a number a caller might act on.
  std::int64_t idle_ms = -1;

  if (it != _allocated.end()) {
    for (const auto& [id, relays] : it->second) {
      for (const auto& relay : {relays.rtp, relays.rtcp}) {
        if (!relay) continue;

        const auto relay_idle = relay->idle_for().count();
        if (idle_ms < 0 || relay_idle < idle_ms) idle_ms = relay_idle;
      }
    }
  }

  const auto idle_seconds = idle_ms < 0 ? std::string("null") : std::to_string(idle_ms / 1000);

  // Per end per stream, RTP only: RTCP is the same ends reporting on it. Which
  // participant an end is cannot be said honestly - the relay learns an end from where its
  // packets come from, and behind a NAT that is not the address its description gave.
  std::string legs = "[";
  if (it != _allocated.end()) {
    for (const auto& [id, relays] : it->second) {
      if (!relays.rtp) continue;
      for (const auto& end : relays.rtp->counts()) {
        if (legs.size() > 1) legs += ",";
        legs += "{\"packets_in\":" + std::to_string(end.packets_in) + ",\"bytes_in\":" + std::to_string(end.bytes_in) +
                ",\"packets_out\":" + std::to_string(end.packets_out) + ",\"bytes_out\":" + std::to_string(end.bytes_out) + "}";
      }
    }
  }
  legs += "]";

  return "{\"call_id\":\"" + call->id + "\",\"engine\":\"builtin\",\"relay_sets\":" + std::to_string(count) + ",\"idle_seconds\":" + idle_seconds +
         ",\"legs\":" + legs + "}";
}

std::optional<std::uint64_t> BuiltinMediaEngine::packets_relayed() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _relay ? _relay->packets_relayed() : 0;
}

}  // namespace athenasip::media
