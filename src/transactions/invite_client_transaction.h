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

// RFC 3261 section 17.1.1 with RFC 6026, the INVITE client transaction.
//
//   Calling -- 1xx --> Proceeding -- 300-699 --> Completed -- timer D --> Terminated
//           -- 2xx ------------------------------> Accepted -- timer M --> Terminated
//
// The transaction ACKs a non-2xx itself (17.1.1.3). The ACK for a 2xx is a separate transaction and is the TU's.
class InviteClientTransaction : public TransactionBase {
 public:
  InviteClientTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers, std::shared_ptr<TimerSource> timer_source,
                          SendFn send, TuFn to_tu);

  // Sends the INVITE and arms timers A and B.
  void start(std::shared_ptr<SIPMessage> invite);

  void receive(std::shared_ptr<SIPMessage> message) override;
  void send(std::shared_ptr<SIPMessage> message) override;

  std::shared_ptr<SIPMessage> last_ack() const { return _ack; }

 protected:
  void _cancel_all_timers() override;

 private:
  void _start_timer_a();
  void _start_timer_b();
  void _start_timer_d();
  void _start_timer_m();

  // RFC 3261 17.1.1.3: the request's Call-ID, From, Request-URI, top Via and CSeq number, the response's To, method ACK.
  std::shared_ptr<SIPMessage> _build_ack(const std::shared_ptr<SIPMessage>& response) const;

  std::shared_ptr<SIPMessage> _request;
  std::shared_ptr<SIPMessage> _ack;

  std::shared_ptr<Timer> _timer_a;
  std::shared_ptr<Timer> _timer_b;
  std::shared_ptr<Timer> _timer_d;
  std::shared_ptr<Timer> _timer_m;

  std::chrono::milliseconds _a_interval{0};
};

}  // namespace athenasip::transactions
