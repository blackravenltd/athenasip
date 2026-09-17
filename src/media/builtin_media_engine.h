//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../rtp/rtp_relay.h"
#include "../types/url.h"
#include "media_engine.h"

namespace athenasip::media {

// The zero-config media engine: a plain RTP relay in this process, bridging two or
// more participants with no ICE, DTLS or transcoding. It is what runs when there is no
// rtpengine, and it only claims the bridge capability.
//
// Configured through its URL, the way the other drivers are:
//   builtin://?public_address=203.0.113.5&bind_address=0.0.0.0&port_min=22000&port_max=23000
class BuiltinMediaEngine : public MediaEngine {
 public:
  BuiltinMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~BuiltinMediaEngine() override;

  std::string get_driver_name() const override;

  bool connect() override;
  void close() override;
  bool is_connected() const override;

  // Bridge only. No conference, no recording, no transcoding.
  Capabilities capabilities() const override;

  Result offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) override;
  Result answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) override;
  bool release(std::shared_ptr<Call> call) override;
  std::string query(std::shared_ptr<Call> call) override;

 private:
  // Rewrites the SDP so this node is the media endpoint for every stream, allocating a
  // relay pair per media description. Ported from the pre-reset SIPCore::_map_media.
  Result _map_media(std::shared_ptr<Call> call, const std::string& sdp_text, const Flags& flags);

  void _apply_url(const std::shared_ptr<types::URL>& url);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::string _bind_address{"0.0.0.0"};
  std::string _public_address{"0.0.0.0"};
  std::uint16_t _port_min{22000};
  std::uint16_t _port_max{23000};

  std::shared_ptr<rtp::RTPRelay> _relay;
  bool _connected = false;

  // Relay sets held per call, so release() can give the ports back.
  mutable std::mutex _mutex;
  std::unordered_map<std::string, std::vector<std::shared_ptr<rtp::RTPRelaySet>>> _allocated;
};

}  // namespace athenasip::media
