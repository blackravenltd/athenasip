//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "non_invite_server_transaction.h"

#include <utility>

namespace athenasip::transactions {

namespace {

bool is_provisional(int code) { return code >= 100 && code < 200; }
bool is_final(int code) { return code >= 200 && code < 700; }

}  // namespace

NonInviteServerTransaction::NonInviteServerTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers,
                                                       std::shared_ptr<TimerSource> timer_source, SendFn send, TuFn to_tu)
    : TransactionBase(std::move(logger), std::move(id), reliable, timers, std::move(timer_source), std::move(send), std::move(to_tu)) {}

void NonInviteServerTransaction::start(std::shared_ptr<SIPMessage> request) {
  _request = std::move(request);
  _set_state(State::Trying);

  _deliver_to_tu(_request);
}

void NonInviteServerTransaction::receive(std::shared_ptr<SIPMessage> message) {
  (void)message;

  switch (_state) {
    case State::Trying:
      // RFC 3261 17.2.2: nothing to send yet, so a retransmission is dropped. The TU must not see the request twice.
      return;

    case State::Proceeding:
    case State::Completed:
      // A retransmission gets the last response again.
      if (_last_response) _transport_send(_last_response);
      return;

    default:
      return;
  }
}

void NonInviteServerTransaction::send(std::shared_ptr<SIPMessage> message) {
  if (!message || !message->header) return;
  if (_state != State::Trying && _state != State::Proceeding) return;

  const int code = message->header->response_code;

  if (is_provisional(code)) {
    _last_response = message;
    _transport_send(message);
    _set_state(State::Proceeding);
    return;
  }

  if (!is_final(code)) return;

  _last_response = message;
  _transport_send(message);
  _set_state(State::Completed);

  _start_timer_j();
}

void NonInviteServerTransaction::_start_timer_j() {
  auto self = std::static_pointer_cast<NonInviteServerTransaction>(shared_from_this());

  // J answers request retransmissions; on a reliable transport there are none and the transaction ends at once.
  const auto delay = _reliable ? std::chrono::milliseconds(0) : _timers.j;

  _timer_j = _start_timer(delay, [self]() { self->_set_state(State::Terminated); });
}

void NonInviteServerTransaction::_cancel_all_timers() { _cancel(_timer_j); }

}  // namespace athenasip::transactions
