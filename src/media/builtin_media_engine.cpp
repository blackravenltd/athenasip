//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "builtin_media_engine.h"

#include <cstdint>
#include <sstream>
#include <utility>

#include "../config.h"
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
    : _logger(std::make_shared<loggers::LoggerScoped>("builtin_media", std::move(logger))), _url(std::move(url)), _public(_logger) {
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

  // Resolves a name before the node serves anything, so the first call has an address.
  _public.set(_public_address);
  _public.start();

  _logger->info("Connected, relaying on " + _bind_address + " as " + _public_address);
  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

void BuiltinMediaEngine::close() {
  if (!_connected) return;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _allocated.clear();
    _emitted.clear();
  }

  _public.stop();

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

// A relay only: plain in, plain out. Mirror is whatever the caller sent.
bool BuiltinMediaEngine::produces(Profile profile) const { return profile == Profile::PlainRtp || profile == Profile::Mirror; }

// The work is synchronous; the handler is still delivered on the caller's executor, as for a networked engine.
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

  // Where this leg said to send its media, read before the rewrite.
  const auto session_address = sdp->has_connection() ? sdp->connection().address : std::string();

  // This node becomes the endpoint for every stream.
  ConnectionInfo relay;
  relay.nettype = "IN";
  relay.addrtype = "IP4";
  relay.address = flags.address.empty() ? _public.current() : flags.address;

  // Decline rather than write an address that would send the far end's media nowhere.
  if (relay.address.empty()) return Result::failure("the public address " + _public_address + " does not resolve to an address");

  sdp->set_connection(relay);

  // RFC 8866 section 5.2: o= carries the originator's address, which anchoring exists to hide. Only the address is
  // replaced; the endpoint's username and session id keep the field globally unique.
  auto origin = sdp->origin();
  origin.nettype = relay.nettype;
  origin.addrtype = relay.addrtype;
  origin.address = relay.address;
  sdp->set_origin(origin);

  auto& participant = call->participants[flags.participant];

  for (auto& media : sdp->media()) {
    const auto id = media.unique_id();

    // RFC 3264 sections 5.1, 6 and 8.2: a port-zero stream is not offered, declined or removed. It keeps its place and
    // gets no relay; a relay port would tell the other end the stream had been accepted. Anything it held is released.
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

      // A stream belongs to the call, not the leg: the second leg to arrive joins the relays already allocated.
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

    // The leg's own record of its streams, for whatever reads a call.
    auto& stream = participant.streams[id];
    if (!stream) {
      stream = std::make_shared<MediaStream>();
      stream->id = std::to_string(id);
    }

    stream->rtp_set = relays.rtp;
    stream->rtcp_set = relays.rtcp;

    // The relay sends to the described address from the start, so a leg that only listens still gets media. RTCP goes to
    // a=rtcp (RFC 3605), or to the port above (RFC 3550 section 11).
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

    media.description.port = relays.rtp->port;

    // Only rewrite a media-level c= that was already there; adding one would change the shape of the offer.
    if (media.has_connection()) media.set_connection(relay);

    // RFC 3605, always written: the relay's RTCP port is not reliably the one above its RTP port.
    const auto rtcp = "rtcp:" + std::to_string(relays.rtcp->port) + " IN IP4 " + relay.address;
    if (!media.set_attribute("rtcp:", rtcp)) media.add_attribute(rtcp);
  }

  _apply_version(call, flags, *sdp);

  return Result::success(sdp->to_string());
}

// RFC 3264 section 8: a changed description carries a higher version, an unchanged one the same. The endpoint's
// version cannot be passed through, because what this node emits (addresses, ports) can change when the endpoint's
// description did not. The first description keeps the endpoint's version; after that the version is this node's.
void BuiltinMediaEngine::_apply_version(const std::shared_ptr<Call>& call, const Flags& flags, SDP& sdp) {
  auto origin = sdp.origin();

  // The body with the version blanked, so comparing two asks whether anything else changed.
  auto blanked = origin;
  blanked.sessionVersion = "0";
  sdp.set_origin(blanked);

  const auto shape = sdp.to_string();

  std::uint64_t version = 0;

  {
    std::lock_guard<std::mutex> lock(_mutex);

    auto& last = _emitted[call->id][flags.participant];

    if (last.shape.empty()) {
      // First description: start from the endpoint's own version.
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

  // The shortest idle time of any relay the call holds: one stream still carrying is a call still up. RTP and RTCP
  // both count, so a held call or a silence-suppressing codec does not read as dead. A call holding nothing reports null.
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

  // Per end per stream, RTP only. Ends are not mapped to participants: the relay knows an end by its source address,
  // which behind NAT is not the address in its description.
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
