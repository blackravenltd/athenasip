//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <memory>
#include <string>

#include "loggers/logger.h"
#include "servers/connection.h"
#include "events/event_system.h"

#include "sip_message.h"

#include "core.h"

using namespace athenasip::loggers;
using namespace athenasip::servers;
using namespace athenasip::events;

namespace athenasip {

class Channel : public std::enable_shared_from_this<Channel> {
 public:
  Channel(std::shared_ptr<Logger>, std::shared_ptr<Core>, std::shared_ptr<Connection>);

  enum State {
    Normal,
    Error,
    Closing,
    Closed,
  };

  void write(std::string message);
  void start();
  void close();

  State state = State::Normal;

  std::shared_ptr<SIPMessage> _incoming_message;
  std::shared_ptr<Subscription> _event_subscription;
  std::shared_ptr<Connection> _connection;

  void send(std::shared_ptr<SIPMessage> message);
  void receive(std::shared_ptr<SIPMessage> message);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Core> _core;


  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  void _schedule_async_read();
  void _schedule_async_write(std::string message);
  bool _append_body();
};

}  // namespace athenasip
