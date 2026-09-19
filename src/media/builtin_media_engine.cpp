//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "builtin_media_engine.h"

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

bool BuiltinMediaEngine::connect() {
  if (_connected) return true;

  _relay = std::make_shared<rtp::RTPRelay>(_logger, _bind_address, _port_min, _port_max);
  _relay->start();
  _connected = true;

  _logger->info("Connected, relaying on " + _bind_address + " as " + _public_address);
  return true;
}

void BuiltinMediaEngine::close() {
  if (!_connected) return;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _allocated.clear();
  }

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

Result BuiltinMediaEngine::offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) {
  // Plain RTP only. An offer needing ICE, DTLS or SRTP belongs to rtpengine.
  if (flags.ice || flags.dtls || flags.srtp) {
    return Result::failure("builtin media engine handles plain RTP only: use rtpengine:// for ICE, DTLS or SRTP");
  }

  return _map_media(std::move(call), sdp, flags);
}

Result BuiltinMediaEngine::answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) {
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

  // Make us the endpoint for everything.
  ConnectionInfo relay;
  relay.nettype = "IN";
  relay.addrtype = "IP4";
  relay.address = _public_address;

  sdp->set_connection(relay);

  auto& participant = call->participants[flags.participant];

  for (auto& media : sdp->media()) {
    const auto id = media.unique_id();

    std::shared_ptr<MediaStream> stream;
    auto existing = participant.streams.find(id);

    if (existing != participant.streams.end()) {
      stream = existing->second;
      _logger->debug("Mapping media, existing stream: " + media.description.to_string());
    } else {
      stream = std::make_shared<MediaStream>();
      stream->id = std::to_string(id);

      auto rtp_set = _relay->allocate_relay_set();
      auto rtcp_set = _relay->allocate_relay_set();

      if (!rtp_set || !rtcp_set) {
        if (rtp_set) _relay->release_relay_set(rtp_set);
        if (rtcp_set) _relay->release_relay_set(rtcp_set);
        return Result::failure("no relay ports available");
      }

      rtp_set->start();
      rtcp_set->start();

      stream->rtp_set = rtp_set;
      stream->rtcp_set = rtcp_set;

      participant.streams.insert({id, stream});

      {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& held = _allocated[call->id];
        held.push_back(rtp_set);
        held.push_back(rtcp_set);
      }

      _logger->debug("Mapping media, created stream: " + media.description.to_string());
    }

    auto rtp_set = stream->rtp_set.lock();
    auto rtcp_set = stream->rtcp_set.lock();
    if (!rtp_set || !rtcp_set) return Result::failure("relay set went away");

    // Point the media at our relay port.
    media.description.port = rtp_set->port;

    // Only rewrite a media-level c= that was already there. Adding one where the far
    // end relied on the session-level line would change the shape of the offer.
    if (media.has_connection()) media.set_connection(relay);

    // And the RTCP attribute at ours, where the far end named one.
    media.set_attribute("rtcp:", "rtcp:" + std::to_string(rtcp_set->port) + " IN IP4 " + _public_address);
  }

  return Result::success(sdp->to_string());
}

bool BuiltinMediaEngine::release(std::shared_ptr<Call> call) {
  if (!call) return false;

  std::vector<std::shared_ptr<rtp::RTPRelaySet>> held;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _allocated.find(call->id);

    // Releasing a call the engine never saw is not an error.
    if (it == _allocated.end()) return true;

    held = std::move(it->second);
    _allocated.erase(it);
  }

  if (_relay) {
    for (const auto& relay_set : held) _relay->release_relay_set(relay_set);
  }

  for (auto& participant : call->participants) participant.streams.clear();

  _logger->debug("Released " + std::to_string(held.size()) + " relay sets for call " + call->id);
  return true;
}

std::string BuiltinMediaEngine::query(std::shared_ptr<Call> call) {
  if (!call) return "{}";

  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _allocated.find(call->id);
  const auto count = (it == _allocated.end()) ? 0u : it->second.size();

  return "{\"call_id\":\"" + call->id + "\",\"engine\":\"builtin\",\"relay_sets\":" + std::to_string(count) + "}";
}

}  // namespace athenasip::media
