//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "transaction.h"
#include "registrar.h"
#include "sip_message.h"

using namespace athenasip::loggers;

namespace athenasip {

Transaction::Transaction(std::shared_ptr<Logger> logger, std::string _id) : id(_id), _logger(std::make_unique<LoggerScoped>("transaction "+_id, logger)) {
  _logger->debug("created");
}
Transaction::~Transaction() {
  _logger->debug("destroyed");
}
void Transaction::start(uint16_t t1_ms) {
  _t1_ms = t1_ms;
  _logger->debug("start");
  _replace_timer();
}

void Transaction::reset_timers() {
  _logger->debug("reset timers");
  _replace_timer();
}

void Transaction::end() {
  _logger->debug("end");
  if(_timer_b_f != nullptr) _timer_b_f->cancel();
  if(!registrar.expired()) registrar.lock()->transaction_unregister(id);
  registrar.reset();
}

void Transaction::parse_message(std::shared_ptr<SIPMessage> message) {
  _logger->debug("parse "+message->header->first_line());
}

void Transaction::_replace_timer() {
  auto self = shared_from_this();

  // Cancel and explicitly free old B/F timer
  if(_timer_b_f != nullptr) _timer_b_f->cancel();
  _timer_b_f.reset();

  // Create new B/F timer
  _timer_b_f = DelayedTask<int>::schedule([this,self]() -> int { 
    _timer_b_f.reset();
    _timeout();
    return 0;
  }, _t1_ms * 10);

  _logger->debug("B/F timer set");
}

void Transaction::_timeout() {
  _logger->debug("B/F timeout");
  end();
}

}  // namespace athenasip