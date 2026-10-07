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

// RFC 3261 section 17.1.2, the non-INVITE client transaction.
//
//   Trying -- 1xx --> Proceeding -- final --> Completed -- timer K --> Terminated
//          -- final ---------------------->
//
// Unlike timer A, timer E is capped at T2, and in Proceeding it fires at T2 flat.
class NonInviteClientTransaction : public TransactionBase {
 public:
  NonInviteClientTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers, std::shared_ptr<TimerSource> timer_source,
                             SendFn send, TuFn to_tu);

  void start(std::shared_ptr<SIPMessage> request);

  void receive(std::shared_ptr<SIPMessage> message) override;
  void send(std::shared_ptr<SIPMessage> message) override;

 protected:
  void _cancel_all_timers() override;

 private:
  void _start_timer_e();
  void _start_timer_f();
  void _start_timer_k();

  std::shared_ptr<SIPMessage> _request;

  std::shared_ptr<Timer> _timer_e;
  std::shared_ptr<Timer> _timer_f;
  std::shared_ptr<Timer> _timer_k;

  std::chrono::milliseconds _e_interval{0};
};

}  // namespace athenasip::transactions
