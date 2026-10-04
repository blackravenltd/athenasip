//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "transaction_base.h"

namespace athenasip::transactions {

// RFC 3261 section 17.2.2, the non-INVITE server transaction.
//
//   Trying -- 1xx from TU --> Proceeding -- final from TU --> Completed -- timer J --> Terminated
//          -- final from TU ------------------------------->
//
// Timer J holds the transaction open after the final response so request retransmissions still get an answer.
class NonInviteServerTransaction : public TransactionBase {
 public:
  NonInviteServerTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers, std::shared_ptr<TimerSource> timer_source,
                             SendFn send, TuFn to_tu);

  void start(std::shared_ptr<SIPMessage> request);

  void receive(std::shared_ptr<SIPMessage> message) override;
  void send(std::shared_ptr<SIPMessage> message) override;

  std::shared_ptr<SIPMessage> last_response() const { return _last_response; }

 protected:
  void _cancel_all_timers() override;

 private:
  void _start_timer_j();

  std::shared_ptr<SIPMessage> _request;
  std::shared_ptr<SIPMessage> _last_response;

  std::shared_ptr<Timer> _timer_j;
};

}  // namespace athenasip::transactions
