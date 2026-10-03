//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "invite_client_transaction.h"

#include <utility>

#include "../headers/cseq_header.h"
#include "../headers/via_header.h"

namespace athenasip::transactions {

namespace {

bool is_provisional(int code) { return code >= 100 && code < 200; }
bool is_2xx(int code) { return code >= 200 && code < 300; }
bool is_final(int code) { return code >= 200 && code < 700; }

}  // namespace

InviteClientTransaction::InviteClientTransaction(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers,
                                                 std::shared_ptr<TimerSource> timer_source, SendFn send, TuFn to_tu)
    : TransactionBase(std::move(logger), std::move(id), reliable, timers, std::move(timer_source), std::move(send), std::move(to_tu)) {}

void InviteClientTransaction::start(std::shared_ptr<SIPMessage> invite) {
  _request = std::move(invite);
  _set_state(State::Calling);

  _transport_send(_request);

  // A retransmits the INVITE and is pointless on a reliable transport. B is the
  // giving-up timer and runs either way.
  if (!_reliable) _start_timer_a();
  _start_timer_b();
}

void InviteClientTransaction::receive(std::shared_ptr<SIPMessage> message) {
  if (!message || !message->header) return;

  const int code = message->header->response_code;

  switch (_state) {
    case State::Calling:
    case State::Proceeding:
      if (is_provisional(code)) {
        // Any response at all stops the retransmissions and the timeout.
        _cancel(_timer_a);
        _cancel(_timer_b);
        _set_state(State::Proceeding);
        _deliver_to_tu(message);
        return;
      }

      if (!is_final(code)) return;

      _cancel(_timer_a);
      _cancel(_timer_b);

      if (is_2xx(code)) {
        // RFC 6026 section 7.2: a 2xx goes straight up and the transaction moves to
        // Accepted, where the 2xx retransmissions still match it. Its ACK is the TU's
        // job, as a separate transaction that may take a different route.
        _deliver_to_tu(message);
        _set_state(State::Accepted);
        _start_timer_m();
        return;
      }

      // 300-699: acknowledge it here, tell the TU once, and stay up long enough to
      // re-acknowledge retransmissions.
      _ack = _build_ack(message);
      _transport_send(_ack);
      _deliver_to_tu(message);

      _set_state(State::Completed);
      _start_timer_d();
      return;

    case State::Completed:
      // A retransmitted final response gets the ACK again and the TU hears nothing.
      if (is_final(code) && !is_2xx(code) && _ack) _transport_send(_ack);
      return;

    case State::Accepted:
      // Every 2xx goes up, retransmissions included: the TU is what acknowledges a 2xx,
      // and a proxy has to send each one on. Anything else is too late to matter.
      if (is_2xx(code)) _deliver_to_tu(message);
      return;

    default:
      return;
  }
}

void InviteClientTransaction::send(std::shared_ptr<SIPMessage> message) {
  // A client transaction sends exactly one request, through start().
  (void)message;
}

std::shared_ptr<SIPMessage> InviteClientTransaction::_build_ack(const std::shared_ptr<SIPMessage>& response) const {
  auto ack = std::make_shared<SIPMessage>();

  ack->header = std::make_shared<SIPHeader>();
  ack->header->type = SIPHeader::Type::Request;
  ack->header->request_method = "ACK";
  ack->header->request_uri = _request->header->request_uri;
  ack->channel = _request->channel;

  // A single Via, equal to the request's topmost.
  if (_request->header->contains("Via")) ack->header->add("Via", _request->header->headers_map["Via"][0]);

  if (_request->header->contains("From")) ack->header->add("From", _request->header->headers_map["From"][0]);
  if (_request->header->contains("Call-ID")) ack->header->add("Call-ID", _request->header->headers_map["Call-ID"][0]);

  // The To comes from the response, because it carries the tag the far end chose.
  if (response->header->contains("To")) {
    ack->header->add("To", response->header->headers_map["To"][0]);
  } else if (_request->header->contains("To")) {
    ack->header->add("To", _request->header->headers_map["To"][0]);
  }

  // Same sequence number, method ACK.
  if (_request->header->contains("CSeq")) {
    auto cseq = _request->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>();
    if (cseq != nullptr) ack->header->add("CSeq", std::make_shared<headers::CSeqHeader>(cseq->sequence, "ACK"));
  }

  if (_request->header->contains("Max-Forwards")) ack->header->add("Max-Forwards", _request->header->headers_map["Max-Forwards"][0]);

  return ack;
}

void InviteClientTransaction::_start_timer_a() {
  auto self = std::static_pointer_cast<InviteClientTransaction>(shared_from_this());

  if (_a_interval.count() == 0) _a_interval = _timers.a;

  _timer_a = _start_timer(_a_interval, [self]() {
    if (self->_state != State::Calling) return;

    self->_transport_send(self->_request);

    // RFC 3261 17.1.1.2: A doubles on every retransmission. Unlike E it is not capped
    // at T2, because timer B ends the attempt first.
    self->_a_interval = self->_a_interval * 2;
    self->_start_timer_a();
  });
}

void InviteClientTransaction::_start_timer_b() {
  auto self = std::static_pointer_cast<InviteClientTransaction>(shared_from_this());

  _timer_b = _start_timer(_timers.b, [self]() {
    if (self->_state != State::Calling) return;

    self->_logger->info("No response to the INVITE, giving up");
    self->_notify_timeout();
    self->_set_state(State::Terminated);
  });
}

void InviteClientTransaction::_start_timer_d() {
  auto self = std::static_pointer_cast<InviteClientTransaction>(shared_from_this());

  // D only exists to re-acknowledge retransmitted final responses, which a reliable
  // transport does not produce.
  const auto delay = _reliable ? std::chrono::milliseconds(0) : _timers.d;

  _timer_d = _start_timer(delay, [self]() { self->_set_state(State::Terminated); });
}

// Timer M ends Accepted, on any transport: the 2xx retransmissions it waits out come from
// the UAS's timer, not this transport's.
void InviteClientTransaction::_start_timer_m() {
  auto self = std::static_pointer_cast<InviteClientTransaction>(shared_from_this());
  _timer_m = _start_timer(_timers.m, [self]() { self->_set_state(State::Terminated); });
}

void InviteClientTransaction::_cancel_all_timers() {
  _cancel(_timer_a);
  _cancel(_timer_b);
  _cancel(_timer_d);
  _cancel(_timer_m);
}

}  // namespace athenasip::transactions
