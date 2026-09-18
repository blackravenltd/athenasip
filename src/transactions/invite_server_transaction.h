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

// RFC 3261 section 17.2.1, the INVITE server transaction.
//
//   Proceeding -- 300-699 from TU --> Completed -- ACK --> Confirmed --> Terminated
//              -- 2xx from TU -----> Terminated
//
// Two things about it are easy to get wrong and both are handled here. A 2xx
// terminates the transaction immediately rather than going to Completed, because a 2xx
// and its ACK are end to end and belong to the TU, not the transaction. And the ACK for
// a non-2xx is absorbed here and never reaches the TU, which is the opposite of the 2xx
// case.
class InviteServerTransaction : public TransactionBase {
 public:
  InviteServerTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers, std::shared_ptr<TimerSource> timer_source,
                          SendFn send, TuFn to_tu);

  // Feeds the transaction the INVITE that created it: sets Proceeding, arms the
  // 100 Trying timer and passes the request up.
  void start(std::shared_ptr<SIPMessage> invite);

  void receive(std::shared_ptr<SIPMessage> message) override;
  void send(std::shared_ptr<SIPMessage> message) override;

  // The last response passed down by the TU, which is what gets retransmitted.
  std::shared_ptr<SIPMessage> last_response() const { return _last_response; }

 protected:
  void _cancel_all_timers() override;

 private:
  void _start_timer_100();
  void _start_timer_g();
  void _start_timer_h();
  void _start_timer_i();

  std::shared_ptr<SIPMessage> _request;
  std::shared_ptr<SIPMessage> _last_response;

  std::shared_ptr<Timer> _timer_100;
  std::shared_ptr<Timer> _timer_g;
  std::shared_ptr<Timer> _timer_h;
  std::shared_ptr<Timer> _timer_i;

  std::chrono::milliseconds _g_interval{0};
};

}  // namespace athenasip::transactions
