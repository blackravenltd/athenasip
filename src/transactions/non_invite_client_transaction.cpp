//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "non_invite_client_transaction.h"

#include <algorithm>
#include <utility>

namespace athenasip::transactions {

namespace {

bool is_provisional(int code) { return code >= 100 && code < 200; }
bool is_final(int code) { return code >= 200 && code < 700; }

}  // namespace

NonInviteClientTransaction::NonInviteClientTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers,
                                                       std::shared_ptr<TimerSource> timer_source, SendFn send, TuFn to_tu)
    : TransactionBase(std::move(logger), std::move(id), reliable, timers, std::move(timer_source), std::move(send), std::move(to_tu)) {}

void NonInviteClientTransaction::start(std::shared_ptr<SIPMessage> request) {
  _request = std::move(request);
  _set_state(State::Trying);

  _transport_send(_request);

  if (!_reliable) _start_timer_e();
  _start_timer_f();
}

void NonInviteClientTransaction::receive(std::shared_ptr<SIPMessage> message) {
  if (!message || !message->header) return;

  const int code = message->header->response_code;

  switch (_state) {
    case State::Trying:
    case State::Proceeding:
      if (is_provisional(code)) {
        _set_state(State::Proceeding);
        _deliver_to_tu(message);

        // RFC 3261 17.1.2.2: in Proceeding, E fires at T2 flat.
        if (!_reliable) {
          _cancel(_timer_e);
          _e_interval = _timers.t2;
          _start_timer_e();
        }
        return;
      }

      if (!is_final(code)) return;

      _cancel(_timer_e);
      _cancel(_timer_f);

      _deliver_to_tu(message);
      _set_state(State::Completed);
      _start_timer_k();
      return;

    case State::Completed:
      // Absorbs retransmitted final responses. The TU has already been told.
      return;

    default:
      return;
  }
}

void NonInviteClientTransaction::send(std::shared_ptr<SIPMessage> message) {
  // A client transaction sends exactly one request, through start().
  (void)message;
}

void NonInviteClientTransaction::_start_timer_e() {
  auto self = std::static_pointer_cast<NonInviteClientTransaction>(shared_from_this());

  if (_e_interval.count() == 0) _e_interval = _timers.e;

  _timer_e = _start_timer(_e_interval, [self]() {
    if (self->_state != State::Trying && self->_state != State::Proceeding) return;

    self->_transport_send(self->_request);

    // In Trying E doubles up to T2; in Proceeding it is already T2.
    self->_e_interval = std::min(self->_e_interval * 2, self->_timers.t2);
    self->_start_timer_e();
  });
}

void NonInviteClientTransaction::_start_timer_f() {
  auto self = std::static_pointer_cast<NonInviteClientTransaction>(shared_from_this());

  _timer_f = _start_timer(_timers.f, [self]() {
    if (self->_state != State::Trying && self->_state != State::Proceeding) return;

    self->_logger->info("No final response, giving up");
    self->_notify_timeout();
    self->_set_state(State::Terminated);
  });
}

void NonInviteClientTransaction::_start_timer_k() {
  auto self = std::static_pointer_cast<NonInviteClientTransaction>(shared_from_this());

  // K absorbs retransmitted final responses, which a reliable transport does not send.
  const auto delay = _reliable ? std::chrono::milliseconds(0) : _timers.k;

  _timer_k = _start_timer(delay, [self]() { self->_set_state(State::Terminated); });
}

void NonInviteClientTransaction::_cancel_all_timers() {
  _cancel(_timer_e);
  _cancel(_timer_f);
  _cancel(_timer_k);
}

}  // namespace athenasip::transactions
