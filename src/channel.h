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
#include <set>
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

// One flow to a peer. Strand-confined: every public method hands over to the Core strand,
// and all state is touched only there.
class Channel : public std::enable_shared_from_this<Channel> {
 public:
  Channel(std::shared_ptr<Logger>, std::shared_ptr<Core>, std::shared_ptr<Connection>);

  enum State {
    Normal,
    Error,
    Closing,
    Closed,
  };

  // Queues raw bytes, in order. For text that is already serialised; send() takes a
  // SIPMessage and serialises it.
  void write(std::string message);

  void start();
  void close();

  // The flow's name: its Core::channel_key, and the flow id a binding records (RFC 5626).
  // Fixed at construction, so it outlives the connection.
  const std::string& flow_id() const { return _flow_id; }

  // The opaque flow token for the user part of this node's Record-Route (RFC 5626 5.1).
  // Sealed by FlowTokens, so the route set does not reveal the far end's address.
  const std::string& flow_token() const { return _flow_token; }

  // Records a subscriber whose REGISTER was authenticated on this flow, for the life of the
  // flow. Only trusted on connection transports: a UDP source address can be spoofed.
  void authenticated_as(const std::string& aor);

  // The node id from the peer's cluster certificate on a mutual TLS flow. Empty for a client.
  std::string peer_node() const { return _connection ? _connection->peer_identity() : std::string(); }
  bool is_authenticated_as(const std::string& aor) const;

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

  std::set<std::string> _authenticated;

  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  // CRLFs seen since the last message, for the keep-alive ping of RFC 5626 4.4.1.
  int _crlf_run = 0;

  // Pending writes, and how much of the front one has gone. One write is in flight at a
  // time: concurrent writes on one socket interleave, and beast's WebSocket refuses them.
  std::deque<std::shared_ptr<std::string>> _write_queue;
  std::size_t _write_offset = 0;
  bool _writing = false;

  void _schedule_async_read();
  void _schedule_async_write(std::string message);

 public:
  // When the flow last carried anything, either way. Core::_flow_sweep uses it to expire
  // UDP flows, which have no close of their own.
  std::chrono::steady_clock::time_point last_activity() const { return _last_activity; }

  // Stamps last_activity from Core::now(), the clock the sweep reads.
  void touch();

 private:
  std::chrono::steady_clock::time_point _last_activity = std::chrono::steady_clock::now();

  void _write_next();
  void _on_write(boost::system::error_code ec, std::size_t length);

  // RFC 3261 18.3: a stream is framed by Content-Length across reads; a datagram is one
  // whole message or nothing.
  void _frame();
  void _frame_stream();
  void _take_keep_alives();
  void _frame_datagram();

  bool _append_body();

  void _send_on_strand(std::shared_ptr<SIPMessage> message);
  void _stamp_via(const std::shared_ptr<SIPMessage>& message);
  void _on_read(boost::system::error_code ec, std::size_t length);
};

}  // namespace athenasip
