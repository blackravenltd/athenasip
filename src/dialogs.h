//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "loggers/logger.h"
#include "sip_message.h"
#include "timer_source.h"
#include "types/dialog.h"

namespace athenasip {

// RFC 3261 section 12, tracked rather than owned.
//
// This is not a transaction user and it does not route: a proxy is transaction-stateful
// and not dialog-stateful (16.1), so every request still reaches its far end on the
// route set it carries, and would do so with this class deleted. What it exists for is
// the thing a proxy alone cannot answer - whether a call is still up. A node that
// anchors media has to release it, a node that writes call records has to close them,
// and an admin API that lists live calls has to have something to list.
//
// It is therefore an observer. Nothing here modifies a message, and nothing upstream
// waits on it.
//
// Strand-confined, like everything else Core owns.
class Dialogs : public std::enable_shared_from_this<Dialogs> {
 public:
  using ChangeFn = std::function<void(const std::shared_ptr<types::Dialog>&)>;

  Dialogs(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<TimerSource> timers);

  // A request arriving from the network, before anything is done with it.
  void observe_request(const std::shared_ptr<SIPMessage>& request);

  // A response going back towards the caller, whether it came from a branch or this node
  // generated it. The request is the one it answers, which is where the caller's half of
  // the dialog comes from.
  void observe_response(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& response);

  // A branch of a fork that failed while the fork goes on (RFC 3261 16.7). It ends that
  // branch's early dialog, if it made one, and not the attempt: the caller's call is over
  // when the final response this node sends it says so, which observe_response is told.
  void observe_branch_failure(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& response);

  // RFC 3261 12.2.2: the dialog a message belongs to, or null.
  std::shared_ptr<types::Dialog> find(const std::shared_ptr<SIPMessage>& message) const;

  // Every dialog this node is still on the path of. A terminated one is gone from here
  // by the time the change callback has returned, so this is what is live and nothing
  // else.
  std::vector<std::shared_ptr<types::Dialog>> all() const;
  std::size_t size() const;

  // Fires on every state change, including termination. Core uses it to keep the Call
  // record and the event bus in step without this class knowing what either is.
  void on_change(ChangeFn fn) { _on_change = std::move(fn); }

  // Drops a dialog from the table, for a caller that has torn it down some other way.
  void terminate(const std::shared_ptr<types::Dialog>& dialog);

 private:
  // The dialogs for one Call-ID. More than one is a fork that more than one branch
  // answered (12.1).
  using Branches = std::vector<std::shared_ptr<types::Dialog>>;

  // The dialog for this Call-ID and To tag, creating it from the caller's half when this
  // is the first time that tag has been seen.
  std::shared_ptr<types::Dialog> _for_tag(const std::string& call_id, const std::string& callee_tag);

  void _changed(const std::shared_ptr<types::Dialog>& dialog);
  void _terminate(const std::shared_ptr<types::Dialog>& dialog, const char* reason);

  // RFC 4028. One timer for the node, set to the next deadline of any dialog that
  // negotiated a session interval, rather than one timer per dialog: the work on each
  // firing is a walk of a table that is already small, and a call is refreshed far more
  // often than it ends.
  void _reschedule_sweep();
  void _sweep();

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<TimerSource> _timers;
  ChangeFn _on_change;

  std::shared_ptr<Timer> _sweep_timer;

  std::unordered_map<std::string, Branches> _by_call_id;
};

}  // namespace athenasip
