//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "invite_server_transaction.h"

#include <algorithm>
#include <utility>

namespace athenasip::transactions {

namespace {

// RFC 3261 17.2.1: if the TU has not responded within 200ms the transaction sends 100 Trying itself.
constexpr std::chrono::milliseconds kTrying100Delay{200};

bool is_provisional(int code) { return code >= 100 && code < 200; }
bool is_2xx(int code) { return code >= 200 && code < 300; }
bool is_final(int code) { return code >= 200 && code < 700; }

}  // namespace

InviteServerTransaction::InviteServerTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers,
                                                 std::shared_ptr<TimerSource> timer_source, SendFn send, TuFn to_tu)
    : TransactionBase(std::move(logger), std::move(id), reliable, timers, std::move(timer_source), std::move(send), std::move(to_tu)) {}

void InviteServerTransaction::start(std::shared_ptr<SIPMessage> invite) {
  _request = std::move(invite);
  _set_state(State::Proceeding);

  _start_timer_100();
  _deliver_to_tu(_request);
}

void InviteServerTransaction::receive(std::shared_ptr<SIPMessage> message) {
  if (!message || !message->header) return;

  const bool is_ack = message->header->request_method == "ACK";

  switch (_state) {
    case State::Proceeding:
      // RFC 3261 17.2.1: a retransmitted INVITE gets the latest provisional response again; the TU does not see it.
      if (!is_ack && _last_response) _transport_send(_last_response);
      return;

    case State::Completed:
      if (is_ack) {
        // The ACK for a non-2xx is absorbed here and never reaches the TU.
        _cancel(_timer_g);
        _cancel(_timer_h);
        _set_state(State::Confirmed);
        _start_timer_i();
        return;
      }

      // A retransmitted INVITE gets the final response again.
      if (_last_response) _transport_send(_last_response);
      return;

    case State::Confirmed:
      // Absorbs ACK retransmissions.
      return;

    case State::Accepted:
      // RFC 6026 section 7.1: a retransmitted INVITE is absorbed; an ACK goes to the TU.
      if (is_ack) _deliver_to_tu(message);
      return;

    default:
      return;
  }
}

void InviteServerTransaction::send(std::shared_ptr<SIPMessage> message) {
  if (!message || !message->header) return;

  const int code = message->header->response_code;

  // In Accepted only the TU's 2xx retransmissions go out, on the connection the INVITE arrived on.
  if (_state == State::Accepted) {
    if (is_2xx(code)) _transport_send(message);
    return;
  }

  if (_state != State::Proceeding && _state != State::Completed) return;

  _last_response = message;
  _cancel(_timer_100);

  if (is_provisional(code)) {
    _transport_send(message);
    return;
  }

  if (!is_final(code)) return;

  _transport_send(message);

  if (is_2xx(code)) {
    // RFC 6026 section 7.1: a 2xx moves the transaction to Accepted. The TU retransmits the 2xx and handles its ACK;
    // both pass through here until timer L.
    _set_state(State::Accepted);
    _start_timer_l();
    return;
  }

  _set_state(State::Completed);

  // G retransmits the final response until the ACK, on an unreliable transport only. H is the timeout and always runs.
  if (!_reliable) _start_timer_g();
  _start_timer_h();
}

void InviteServerTransaction::_start_timer_100() {
  auto self = std::static_pointer_cast<InviteServerTransaction>(shared_from_this());

  _timer_100 = _start_timer(kTrying100Delay, [self]() {
    if (self->_state != State::Proceeding || self->_last_response) return;

    auto trying = self->_request->generate_response();
    trying->header->response_code = 100;
    trying->header->response_message = "Trying";

    self->_last_response = trying;
    self->_transport_send(trying);
  });
}

void InviteServerTransaction::_start_timer_g() {
  auto self = std::static_pointer_cast<InviteServerTransaction>(shared_from_this());

  if (_g_interval.count() == 0) _g_interval = _timers.g;

  _timer_g = _start_timer(_g_interval, [self]() {
    if (self->_state != State::Completed) return;

    if (self->_last_response) self->_transport_send(self->_last_response);

    // RFC 3261 17.2.1: G doubles up to T2.
    self->_g_interval = std::min(self->_g_interval * 2, self->_timers.t2);
    self->_start_timer_g();
  });
}

void InviteServerTransaction::_start_timer_h() {
  auto self = std::static_pointer_cast<InviteServerTransaction>(shared_from_this());

  _timer_h = _start_timer(_timers.h, [self]() {
    // RFC 3261 17.2.1: no ACK came, so the TU is told.
    self->_logger->info("No ACK for the final response, giving up");
    self->_notify_timeout();
    self->_set_state(State::Terminated);
  });
}

void InviteServerTransaction::_start_timer_i() {
  auto self = std::static_pointer_cast<InviteServerTransaction>(shared_from_this());

  // I absorbs ACK retransmissions; on a reliable transport there are none and the transaction ends at once.
  const auto delay = _reliable ? std::chrono::milliseconds(0) : _timers.i;

  _timer_i = _start_timer(delay, [self]() { self->_set_state(State::Terminated); });
}

void InviteServerTransaction::_start_timer_l() {
  auto self = std::static_pointer_cast<InviteServerTransaction>(shared_from_this());
  _timer_l = _start_timer(_timers.l, [self]() { self->_set_state(State::Terminated); });
}

void InviteServerTransaction::_cancel_all_timers() {
  _cancel(_timer_100);
  _cancel(_timer_g);
  _cancel(_timer_h);
  _cancel(_timer_i);
  _cancel(_timer_l);
}

}  // namespace athenasip::transactions
