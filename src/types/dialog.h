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

// RFC 3261 section 12. A dialog is a peer-to-peer relationship between two user agents,
// and this node is neither of them: it is on the path because it record-routed, and what
// it keeps is a record of a dialog it does not own. Section 16.1 is explicit that a
// proxy is transaction-stateful rather than dialog-stateful, so nothing routes on what
// is here. The node keeps it because it anchors media, writes call records and shows
// live calls, all of which need to know when a call ended.
//
// That vantage point is why the two ends are named for their part in the call rather
// than the RFC's "local" and "remote". Those are written from inside one UA, and a proxy
// using them would have to pick an end to pretend to be. Everything else is 12.1.1's
// list, kept as it is seen.
struct Dialog {
  enum class State {
    // 12.1: the callee has responded, but not with a final response. There may be
    // several of these for one call if the request forked.
    Early,
    // Confirmed by a 2xx.
    Confirmed,
    // Ended by a BYE, or by a final response that was not a 2xx.
    Terminated,
  };

  // 12.1.1: the dialog ID is the Call-ID and the two tags. The callee's is not known
  // until it answers, so a call attempt exists here before its dialog does.
  std::string call_id;
  std::string caller_tag;
  std::string callee_tag;

  std::shared_ptr<SIPIdentity> caller;
  std::shared_ptr<SIPIdentity> callee;

  // 12.1.1 remote target: the Contact each end offered, which is where a request in this
  // dialog goes once its route set has been walked.
  std::shared_ptr<SIPUri> caller_target;
  std::shared_ptr<SIPUri> callee_target;

  // 12.1.1 route set: the Record-Route values, in the order they were recorded. This
  // node put itself in this list, which is what brings the BYE back through it.
  std::vector<std::shared_ptr<SIPUri>> route_set;

  // 12.2.1.1: the sequence number each end has reached. A request below what has been
  // seen is out of order (12.2.2).
  std::uint64_t caller_cseq = 0;
  std::uint64_t callee_cseq = 0;

  // 12.1.1: a dialog is secure when the INVITE arrived over TLS and its Request-URI was
  // a sips URI. Both, not either: TLS on one hop says nothing about the rest of the path.
  bool secure = false;

  State state = State::Early;

  std::time_t created_at = 0;
  std::time_t confirmed_at = 0;
  std::time_t terminated_at = 0;

  // RFC 4028, as negotiated in the 2xx. Zero when the call carries no session timer,
  // which is every call between endpoints that did not offer one, and which is why
  // nothing expires by default: a proxy that timed out a call whose ends never agreed to
  // a timer would be ending a call that is still up.
  std::uint32_t session_interval = 0;

  // Which end refreshes, "uac" or "uas", or empty when the 2xx did not say. Recorded
  // because it is what says whose silence is meaningful, not because this node refreshes
  // anything - it is a proxy, and the refresh is an endpoint's to send.
  std::string refresher;

  // When the session lapses if nothing refreshes it. On the steady clock, not the wall
  // one: a session interval is a duration, and a clock step backwards must not extend a
  // call by an hour.
  std::chrono::steady_clock::time_point session_deadline{};

  // 12.1.1: Call-ID plus both tags. Empty while the callee has not answered, because
  // until then there is no dialog to have an identifier.
  std::string id() const;

  // 12.2.2, read from the side of a proxy: a request in this dialog may come from either
  // end, so the two tags arrive in either order and both orders are the same dialog.
  bool matches(const std::string& message_call_id, const std::string& from_tag, const std::string& to_tag) const;

  // Which end a request came from, which is what says whose CSeq and whose target it
  // carries.
  bool is_from_caller(const std::string& from_tag) const;
};

}  // namespace athenasip::types
