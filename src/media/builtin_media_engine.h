//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
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
// Selected by its URL, like every other driver, and configured through its own section
// of the config:
//
//   media:
//     url: builtin://
//     builtin:
//       public_address: 203.0.113.5
//       bind_address: 0.0.0.0
//       port_min: 22000
//       port_max: 23000
//
// The URL query form (builtin://?public_address=...) still works and is read first, so
// a one-line config needs nothing else.
class BuiltinMediaEngine : public MediaEngine {
 public:
  BuiltinMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~BuiltinMediaEngine() override;

  std::string name() const override;
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;

  void connect(plugins::Executor on, plugins::StatusHandler handler) override;
  void close() override;
  bool is_connected() const override;

  // Bridge only. No conference, no recording, no transcoding.
  Capabilities capabilities() const override;

  void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) override;

 private:
  // The work itself, synchronous: this engine relays in-process and genuinely has the
  // answer at once. The public methods are the contract, and deliver these on the
  // caller's executor.
  Result _offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags);
  Result _answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags);
  bool _release(std::shared_ptr<Call> call);
  std::string _query(std::shared_ptr<Call> call);

  // Rewrites the SDP so this node is the media endpoint for every stream, allocating a
  // relay pair per media description. Ported from the pre-reset SIPCore::_map_media.
  Result _map_media(std::shared_ptr<Call> call, const std::string& sdp_text, const Flags& flags);

  // RFC 3264 section 8's version rule, applied to what this node emits rather than to
  // what it was given.
  void _apply_version(const std::shared_ptr<Call>& call, const Flags& flags, SDP& sdp);

  void _apply_url(const std::shared_ptr<types::URL>& url);

  // Port range sanity, applied after the URL and again after the config section.
  void _validate_port_range();

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::string _bind_address{"0.0.0.0"};
  std::string _public_address{"0.0.0.0"};
  std::uint16_t _port_min{22000};
  std::uint16_t _port_max{23000};

  std::shared_ptr<rtp::RTPRelay> _relay;
  bool _connected = false;

  // What one media stream is relayed through. Both legs are given the same pair: the
  // relay learns where a leg is from the first packet it sends and forwards to every
  // other leg that has, so one port is the bridge and two ports would be two sinks.
  struct StreamRelays {
    std::shared_ptr<rtp::RTPRelaySet> rtp;
    std::shared_ptr<rtp::RTPRelaySet> rtcp;
  };

  // What was last handed to one leg, so that the version this node emits obeys RFC
  // 3264 section 8: a description that changed must carry a higher version, and one
  // that did not must carry the same. The endpoint's own version cannot answer it,
  // because what this node emits is not what the endpoint sent.
  struct LastEmitted {
    // The body with the version field blanked, so that comparing two of them asks
    // whether anything but the version changed.
    std::string shape;
    std::uint64_t version = 0;
  };

  // Held per call and then per stream, which is the granularity a re-offer matches on
  // and the granularity release() gives back.
  mutable std::mutex _mutex;
  std::unordered_map<std::string, std::unordered_map<std::int64_t, StreamRelays>> _allocated;
  std::unordered_map<std::string, std::unordered_map<std::size_t, LastEmitted>> _emitted;
};

}  // namespace athenasip::media
