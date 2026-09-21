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
#include <deque>
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

  // Raw bytes out, in the order they were given. For what is already a SIP message -
  // a serialised response, or a test driving the transport - where send() is for a
  // SIPMessage this channel still has to stamp and serialise.
  void write(std::string message);

  void start();
  void close();

  // The name this flow is known by, everywhere: the key the registry files it under,
  // the key channel_find answers to, and the flow id a binding learned over this
  // channel records (RFC 5626). Taken once at construction, because close() gives up
  // the connection and a closing channel still has to say which flow it was.
  const std::string& flow_id() const { return _flow_id; }

  State state = State::Normal;

  std::shared_ptr<SIPMessage> _incoming_message;
  std::shared_ptr<Connection> _connection;

  void send(std::shared_ptr<SIPMessage> message);
  void receive(std::shared_ptr<SIPMessage> message);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Core> _core;

  std::string _flow_id;

  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  // What is waiting to go out, and how much of the front one already has. Both are
  // strand state: a message is queued on the strand and the queue is advanced on the
  // strand, so only the write itself happens on the connection's executor.
  //
  // One write is in flight at a time. Two outstanding writes on one socket interleave
  // their bytes, which for SIP means two messages arriving as neither, and beast's
  // WebSocket stream refuses the second outright.
  std::deque<std::shared_ptr<std::string>> _write_queue;
  std::size_t _write_offset = 0;
  bool _writing = false;

  void _schedule_async_read();
  void _schedule_async_write(std::string message);

  // Strand-confined: start the next write, or stop when there is nothing left.
  void _write_next();
  void _on_write(boost::system::error_code ec, std::size_t length);

  // RFC 3261 18.3, which is two rules and not one: a stream is framed by Content-Length
  // across as many reads as it takes, and a datagram is one whole message or nothing.
  void _frame();
  void _frame_stream();
  void _frame_datagram();

  bool _append_body();

  // Strand-confined bodies behind the public hand-offs.
  void _send_on_strand(std::shared_ptr<SIPMessage> message);
  void _stamp_via(const std::shared_ptr<SIPMessage>& message);
  void _on_read(boost::system::error_code ec, std::size_t length);
};

}  // namespace athenasip
