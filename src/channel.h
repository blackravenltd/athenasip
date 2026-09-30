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
#include <chrono>
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

  // An opaque name for this flow, for the Record-Route this node writes (RFC 5626
  // section 5.1 puts a flow token in the user part for exactly this). It is random
  // rather than derived, because the flow id is the far end's address and a token that
  // spelled it out would hand one end of an anchored call the other end's address in
  // the route set - the thing anchoring the media is there to prevent.
  //
  // It lives as long as the channel does, which is as long as it is any use: a token
  // naming a flow that has closed names nothing, and the socket does not survive a
  // restart either.
  const std::string& flow_token() const { return _flow_token; }

  State state = State::Normal;

  std::shared_ptr<SIPMessage> _incoming_message;
  std::shared_ptr<Connection> _connection;

  void send(std::shared_ptr<SIPMessage> message);
  void receive(std::shared_ptr<SIPMessage> message);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Core> _core;

  std::string _flow_id;
  std::string _flow_token;

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

 public:
  // When this flow last carried anything, either way.
  //
  // It exists for UDP. A TCP, TLS or WebSocket flow ends when its socket does, and the
  // read completing with an error is what tears the channel down. UDP has no socket per
  // peer and no close to wait for, so a flow made by one datagram would otherwise live
  // as long as the process - see Core::_flow_sweep.
  std::chrono::steady_clock::time_point last_activity() const { return _last_activity; }

  // Defined where Core is a complete type. It reads Core's clock rather than steady_clock
  // so that the stamp and the sweep that reads it are the same clock.
  void touch();

 private:
  std::chrono::steady_clock::time_point _last_activity = std::chrono::steady_clock::now();

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
