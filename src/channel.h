//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <memory>
#include <string>

#include "core.h"
#include "events/event_system.h"
#include "loggers/logger.h"
#include "servers/connection.h"
#include "sip_message.h"

using namespace athenasip::loggers;
using namespace athenasip::servers;
using namespace athenasip::events;

namespace athenasip {

// A Channel is strand-confined: every method below hands over to the Core strand, and
// everything it owns is touched only from there. Servers, and the connection's own
// io_context thread, reach it only through that hand-off.
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

  // Strand-confined bodies behind the public hand-offs.
  void _send_on_strand(std::shared_ptr<SIPMessage> message);
  void _stamp_via(const std::shared_ptr<SIPMessage>& message);
  void _on_read(boost::system::error_code ec, std::size_t length);
};

}  // namespace athenasip
