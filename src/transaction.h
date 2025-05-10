//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "delayed_task.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"

using namespace athenasip::loggers;

namespace athenasip {

class Session;
class Registrar;
class SIPMessage;

class Transaction : public std::enable_shared_from_this<Transaction>  {
 public:
  enum Direction {
    Incoming,
    Outgoing,
  };

  enum Type {
    Unknown,
    INVITE,
    NonINVITE,
    ServerINVITE,
  };

  Transaction(std::shared_ptr<Logger> logger, std::string _id);
  ~Transaction();

  std::string id;
  std::shared_ptr<Session> session;
  std::weak_ptr<Registrar> registrar;

  Direction direction;
  Type type = Type::Unknown;

  void start(uint16_t t1_ms);
  void reset_timers();
  void end();
  void parse_message(std::shared_ptr<SIPMessage> message);

protected:
  std::shared_ptr<Logger> _logger;
  uint16_t _t1_ms;
  std::shared_ptr<DelayedTask<int>> _timer_b_f;

  void _replace_timer();
  void _timeout();
};

}  // namespace athenasip