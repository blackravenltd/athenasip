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

// OPTIONS to registered clients, on the interval their realm's behaviour gives: Asterisk's
// qualify and Kamailio's nathelper ping. Each probe goes down the flow the client
// registered on, which keeps a NAT's mapping for it open, and the answer says two things:
// that the client is still there, and - when a 200 carries a session description, as RFC
// 3261 11.2 allows - what media it takes. The second is the client's own word, so the
// proxy ranks it with what a leg says in a call rather than with anybody's guess.
//
// Only the node holding a flow can probe down it, so this is per node and in memory.
// Everything runs on Core's strand.
class Qualifier : public std::enable_shared_from_this<Qualifier> {
 public:
  struct Probe {
    std::string aor;
    std::shared_ptr<types::SIPUri> contact;
    std::string flow_id;
    std::uint32_t interval = 0;
    std::time_t expires_at = 0;

    // What the last session description the client sent back said, if one did.
    std::optional<media::Profile> said;

    // Probes in a row that nothing answered, and when something last did.
    std::uint32_t unanswered = 0;
    std::time_t answered_at = 0;

    // One Call-ID and From tag for the binding's probes, with the CSeq counting up, so a
    // client sees one conversation rather than a new stranger every interval.
    std::string call_id;
    std::string from_tag;
    std::uint32_t cseq = 0;

    std::shared_ptr<Timer> timer;
  };

  Qualifier(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Start or restart probing a binding: once now, then every interval seconds until it
  // expires or its flow goes. An interval of zero stops it.
  void watch(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact, const std::string& flow_id, std::uint32_t interval,
             std::uint32_t expires_seconds);

  // The binding is gone.
  void forget(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact);

  // What the client on this flow said its media is, the last time a probe asked.
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
