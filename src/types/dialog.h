//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "sip_identity.h"
#include "sip_uri.h"

namespace athenasip::types {

// RFC 3261 section 12: this node's record of a dialog it is on the path of but does not own. A proxy is
// transaction-stateful, not dialog-stateful (16.1), so nothing routes on this; it is kept for media anchoring, call
// records and live calls, which need to know when a call ended.
//
// The ends are named caller and callee, not the RFC's "local" and "remote", which are a UA's view. The rest is
// 12.1.1's list.
struct Dialog {
  enum class State {
    // 12.1: a non-final response from the callee. A forked request may have several.
    Early,
    // Confirmed by a 2xx.
    Confirmed,
    // Ended by a BYE, or by a final response that was not a 2xx.
    Terminated,
  };

  // 12.1.1: the dialog ID is the Call-ID and the two tags. The callee's is unknown until it answers.
  std::string call_id;
  std::string caller_tag;
  std::string callee_tag;

  // The peer node that forwarded the request that began this dialog. Empty on the node the caller reached, which
  // owns the call's record.
  std::string from_node;

  std::shared_ptr<SIPIdentity> caller;
  std::shared_ptr<SIPIdentity> callee;

  // 12.1.1 remote target: the Contact each end offered, where an in-dialog request goes after its route set.
  std::shared_ptr<SIPUri> caller_target;
  std::shared_ptr<SIPUri> callee_target;

  // 12.1.1 route set: the Record-Route values in the order recorded, this node's own among them.
  std::vector<std::shared_ptr<SIPUri>> route_set;

  // 12.2.1.1: the sequence number each end has reached. A lower one is out of order (12.2.2).
  std::uint64_t caller_cseq = 0;
  std::uint64_t callee_cseq = 0;

  // 12.1.1: secure only when the INVITE arrived over TLS and its Request-URI was a sips URI.
  bool secure = false;

  State state = State::Early;

  std::time_t created_at = 0;
  std::time_t confirmed_at = 0;
  std::time_t terminated_at = 0;

  // RFC 4028, as negotiated in the 2xx. Zero when the call has no session timer, in which case nothing expires.
  std::uint32_t session_interval = 0;

  // Which end refreshes, "uac" or "uas", or empty when the 2xx did not say. This node never refreshes; it is a proxy.
  std::string refresher;

  // When the session lapses unless refreshed. Steady clock, so a wall-clock step cannot extend a call.
  std::chrono::steady_clock::time_point session_deadline{};

  // When the callee answered, on the steady clock: what a maximum call duration is measured from. `confirmed_at`
  // is the wall-clock time for the call record.
  std::chrono::steady_clock::time_point confirmed_monotonic{};

  // 12.1.1: Call-ID plus both tags. Empty until the callee has answered.
  std::string id() const;

  // 12.2.2, seen from a proxy: either end may send, so both orders of the tags match.
  bool matches(const std::string& message_call_id, const std::string& from_tag, const std::string& to_tag) const;

  // Which end a request came from, and so whose CSeq and target it carries.
  bool is_from_caller(const std::string& from_tag) const;
};

}  // namespace athenasip::types
