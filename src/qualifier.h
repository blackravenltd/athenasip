//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "media/media_profile.h"
#include "sip_message.h"
#include "timer_source.h"
#include "types/sip_uri.h"

namespace athenasip {

class Core;

// Sends OPTIONS to registered clients at the interval their realm's behaviour sets, like
// Asterisk's qualify. Each probe goes down the flow the client registered on, keeping its
// NAT mapping open. The answer shows the client is alive and, when a 200 carries SDP (RFC
// 3261 11.2), what media it takes.
//
// Only the node holding a flow can probe it, so state is per node and in memory. Runs on
// Core's strand.
class Qualifier : public std::enable_shared_from_this<Qualifier> {
 public:
  struct Probe {
    std::string aor;
    std::shared_ptr<types::SIPUri> contact;
    std::string flow_id;
    std::uint32_t interval = 0;
    std::time_t expires_at = 0;

    // The media profile stated by the client's last SDP answer, if any.
    std::optional<media::Profile> said;

    // Consecutive unanswered probes, and when one was last answered.
    std::uint32_t unanswered = 0;
    std::time_t answered_at = 0;

    // One Call-ID and From tag per binding with an incrementing CSeq, so the client sees a
    // single conversation.
    std::string call_id;
    std::string from_tag;
    std::uint32_t cseq = 0;

    std::shared_ptr<Timer> timer;
  };

  Qualifier(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Starts or restarts probing a binding: once now, then every interval seconds until it
  // expires or its flow closes. An interval of zero stops it.
  void watch(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact, const std::string& flow_id, std::uint32_t interval,
             std::uint32_t expires_seconds);

  // Stops probing a binding.
  void forget(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact);

  // What the client on this flow last stated its media to be.
  std::optional<media::Profile> said(const std::string& flow_id) const;

  std::vector<Probe> list() const;

 private:
  void _schedule(const std::string& key, std::chrono::milliseconds delay);
  void _probe(const std::string& key);
  void _on_answer(const std::string& key, const std::shared_ptr<SIPMessage>& response);
  void _on_silence(const std::string& key);

  std::shared_ptr<SIPMessage> _options_for(Probe& probe, const std::string& transport, const std::string& host, std::uint16_t port) const;

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  std::map<std::string, Probe> _probes;
};

}  // namespace athenasip
