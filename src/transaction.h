//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "delayed_task.h"
#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"

#include "sip_message.h"
#include "core.h"

using namespace athenasip::loggers;
using namespace athenasip::headers;

namespace athenasip {

class Transaction : public std::enable_shared_from_this<Transaction> {
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

  Transaction(std::shared_ptr<Logger> logger, std::shared_ptr<Channel> channel, std::shared_ptr<Core> core, Direction direction, std::string _id);
  ~Transaction();

  std::string id;

  Direction direction;
  Type type = Type::Unknown;

  void start(uint16_t t1_ms);
  void reset_timers();
  void end();

  void process_register(std::shared_ptr<SIPMessage> message);
  void process_invite(std::shared_ptr<SIPMessage> message);
  void process_ack(std::shared_ptr<SIPMessage> message);
  void process_unknown(std::shared_ptr<SIPMessage> message);

  void receive_message(std::shared_ptr<SIPMessage> message);
  void send_message(std::shared_ptr<SIPMessage> message);
  void send_401_unauthorized(std::shared_ptr<SIPMessage> message);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Channel> _channel;
  std::shared_ptr<Core> _core;
  Direction _direction;

  uint16_t _t1_ms;
  std::shared_ptr<DelayedTask<int>> _timer_b_f;
  bool _ended = false;

  void _replace_timer();
  void _timeout();
};

}  // namespace athenasip