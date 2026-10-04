//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../rtp/rtp_relay.h"
#include "../types/url.h"
#include "media_engine.h"
#include "public_address.h"

namespace athenasip::media {

// The zero-config engine: a plain RTP relay in this process, bridging two or more participants with no ICE, DTLS, SRTP
// or transcoding. Configured through its own section:
//
//   media:
//     url: builtin://
//     builtin:
//       public_address: 203.0.113.5
//       bind_address: 0.0.0.0
//       port_min: 22000
//       port_max: 23000
//
// The same keys are also read from the URL query (builtin://?public_address=...); the config section overrides them.
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

  Capabilities capabilities() const override;
  bool produces(Profile profile) const override;

  void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) override;
  std::optional<std::uint64_t> packets_relayed() const override;

 private:
  // The work itself, synchronous because the relay is in-process. The public methods deliver these on the caller's executor.
  Result _offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags);
  Result _answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags);
  bool _release(std::shared_ptr<Call> call);
  std::string _query(std::shared_ptr<Call> call);

  // Rewrites the SDP so this node is the media endpoint for every stream, allocating a relay pair per media description.
  Result _map_media(std::shared_ptr<Call> call, const std::string& sdp_text, const Flags& flags);

  // RFC 3264 section 8: versions what this node emits.
  void _apply_version(const std::shared_ptr<Call>& call, const Flags& flags, SDP& sdp);

  void _apply_url(const std::shared_ptr<types::URL>& url);

  // Falls back to the default range when port_max is not above port_min.
  void _validate_port_range();

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::string _bind_address{"0.0.0.0"};
  std::string _public_address{"0.0.0.0"};

  // May be a name (public_address.h).
  PublicAddress _public;
  std::uint16_t _port_min{22000};
  std::uint16_t _port_max{23000};

  std::shared_ptr<rtp::RTPRelay> _relay;
  bool _connected = false;

  // The relays for one media stream. Both legs share the pair: a relay learns each leg from its first packet and
  // forwards to every other, so one port is the bridge.
  struct StreamRelays {
    std::shared_ptr<rtp::RTPRelaySet> rtp;
    std::shared_ptr<rtp::RTPRelaySet> rtcp;
  };

  // What was last emitted to one leg, for RFC 3264 section 8: a changed description carries a higher version, an
  // unchanged one the same.
  struct LastEmitted {
    // The body with the version blanked, so comparing two asks whether anything else changed.
    std::string shape;
    std::uint64_t version = 0;
  };

  // Keyed by call id, then by stream (_allocated) or participant (_emitted).
  mutable std::mutex _mutex;
  std::unordered_map<std::string, std::unordered_map<std::int64_t, StreamRelays>> _allocated;
  std::unordered_map<std::string, std::unordered_map<std::size_t, LastEmitted>> _emitted;
};

}  // namespace athenasip::media
