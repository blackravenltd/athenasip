//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "../config.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../sip_message.h"
#include "../timer_source.h"

namespace athenasip::transactions {

// The RFC 3261 section 17 states; not every machine uses every one. Accepted is RFC 6026's: an INVITE transaction
// that has seen a 2xx, kept for 64*T1 so 2xx retransmissions and their ACKs still have a transaction to match.
enum class State { Calling, Trying, Proceeding, Completed, Confirmed, Accepted, Terminated };

inline std::string state_to_string(State state) {
  switch (state) {
    case State::Calling:
      return "Calling";
    case State::Trying:
      return "Trying";
    case State::Proceeding:
      return "Proceeding";
    case State::Completed:
      return "Completed";
    case State::Confirmed:
      return "Confirmed";
    case State::Accepted:
      return "Accepted";
    case State::Terminated:
      return "Terminated";
  }
  return "Unknown";
}

// The section 17 timer values. T1 is the RTT estimate, T2 the retransmission ceiling and T4 the longest a message
// lingers in the network; the rest are multiples of those.
struct Timers {
  std::chrono::milliseconds t1{500};
  std::chrono::milliseconds t2{4000};
  std::chrono::milliseconds t4{5000};

  std::chrono::milliseconds a{500};    // INVITE client retransmit, doubles to T2
  std::chrono::milliseconds b{32000};  // INVITE client timeout, 64*T1
  std::chrono::milliseconds d{32000};  // INVITE client wait for response retransmits
  std::chrono::milliseconds e{500};    // non-INVITE client retransmit
  std::chrono::milliseconds f{32000};  // non-INVITE client timeout
  std::chrono::milliseconds g{500};    // INVITE server response retransmit
  std::chrono::milliseconds h{32000};  // INVITE server wait for ACK
  std::chrono::milliseconds i{5000};   // INVITE server wait for ACK retransmits
  std::chrono::milliseconds j{32000};  // non-INVITE server wait for request retransmits
  std::chrono::milliseconds k{5000};   // non-INVITE client wait for response retransmits
  std::chrono::milliseconds l{32000};  // INVITE server Accepted, 64*T1 (RFC 6026)
  std::chrono::milliseconds m{32000};  // INVITE client Accepted, 64*T1 (RFC 6026)

  static Timers from_config(const Config& config) {
    Timers timers;
    timers.t1 = std::chrono::milliseconds(config.sip_timer_t1_rtt_ms);
    timers.t2 = std::chrono::milliseconds(config.sip_timer_t2_max_retransmit_interval_ms);
    timers.t4 = std::chrono::milliseconds(config.sip_timer_t4_network_propagation_ms);

    timers.a = timers.t1 * config.sip_timer_a_invite_initial;
    timers.b = timers.t1 * config.sip_timer_b_invite_timeout;
    timers.d = timers.t1 * config.sip_timer_d_invite_duration;
    timers.e = timers.t1 * config.sip_timer_e_non_invite_initial;
    timers.f = timers.t1 * config.sip_timer_f_non_invite_timeout;
    timers.g = timers.t1 * config.sip_timer_g_server_invite_initial;
    timers.h = timers.t1 * config.sip_timer_h_server_invite_timeout;
    timers.i = timers.t4 * config.sip_timer_i_server_invite_duration;
    timers.j = timers.t1 * config.sip_timer_j_server_non_invite_duration;
    timers.k = timers.t4 * config.sip_timer_k_non_invite_duration;
    timers.l = timers.t1 * 64;
    timers.m = timers.t1 * 64;

    return timers;
  }
};

// One RFC 3261 section 17 transaction, between the transport and the transaction user. It absorbs retransmissions
// so the TU sees each request once, and retransmits on the TU's behalf. On a reliable transport the retransmission
// timers A, E and G do not run and the waiting timers D, I, J and K are zero.
class TransactionBase : public std::enable_shared_from_this<TransactionBase> {
 public:
  // Out to the transport.
  using SendFn = std::function<void(std::shared_ptr<SIPMessage>)>;
  // Up to the transaction user.
  using TuFn = std::function<void(std::shared_ptr<SIPMessage>)>;
  // The transaction has reached Terminated and can be forgotten.
  using TerminatedFn = std::function<void(const std::string& id)>;
  // No answer came in time. RFC 3261 requires the TU be told.
  using TimeoutFn = std::function<void()>;

  TransactionBase(std::shared_ptr<loggers::Logger> logger, std::string id, bool reliable, Timers timers, std::shared_ptr<TimerSource> timer_source, SendFn send,
                  TuFn to_tu)
      : _logger(std::make_shared<loggers::LoggerScoped>("transaction " + id, std::move(logger))),
        _id(std::move(id)),
        _reliable(reliable),
        _timers(timers),
        _timer_source(std::move(timer_source)),
        _send(std::move(send)),
        _to_tu(std::move(to_tu)) {}

  virtual ~TransactionBase() = default;

  const std::string& id() const { return _id; }
  State state() const { return _state; }
  bool is_reliable() const { return _reliable; }
  const Timers& timers() const { return _timers; }

  void on_terminated(TerminatedFn fn) { _on_terminated = std::move(fn); }
  void on_timeout(TimeoutFn fn) { _on_timeout = std::move(fn); }

  // A message from the transport for this transaction.
  virtual void receive(std::shared_ptr<SIPMessage> message) = 0;

  // A message from the transaction user to send through this transaction.
  virtual void send(std::shared_ptr<SIPMessage> message) = 0;

  // Terminates from any state.
  void terminate() {
    if (_state == State::Terminated) return;

    _cancel_all_timers();
    _set_state(State::Terminated);
  }

 protected:
  void _set_state(State state) {
    if (_state == state) return;

    _logger->debug(state_to_string(_state) + " -> " + state_to_string(state));
    _state = state;

    if (_state == State::Terminated) {
      _cancel_all_timers();
      if (_on_terminated) _on_terminated(_id);
    }
  }

  std::shared_ptr<Timer> _start_timer(std::chrono::milliseconds delay, std::function<void()> fn) {
    auto self = shared_from_this();
    return _timer_source->schedule(delay, [self, fn = std::move(fn)]() {
      if (self->_state == State::Terminated) return;
      fn();
    });
  }

  static void _cancel(std::shared_ptr<Timer>& timer) {
    if (timer) {
      timer->cancel();
      timer.reset();
    }
  }

  void _transport_send(const std::shared_ptr<SIPMessage>& message) {
    if (_send) _send(message);
  }

  void _deliver_to_tu(const std::shared_ptr<SIPMessage>& message) {
    if (_to_tu) _to_tu(message);
  }

  void _notify_timeout() {
    if (_on_timeout) _on_timeout();
  }

  virtual void _cancel_all_timers() = 0;

  std::shared_ptr<loggers::Logger> _logger;
  std::string _id;
  bool _reliable;
  Timers _timers;
  std::shared_ptr<TimerSource> _timer_source;

  SendFn _send;
  TuFn _to_tu;
  TerminatedFn _on_terminated;
  TimeoutFn _on_timeout;

  State _state = State::Trying;
};

}  // namespace athenasip::transactions
