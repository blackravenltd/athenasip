//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <vector>
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

  // The far end's address, without the port: what a source is limited and challenged as (AuthGuard).
  std::string source_address() const { return _connection ? _connection->remote_endpoint().address().to_string() : std::string(); }

  bool is_authenticated_as(const std::string& aor) const;

  // Whether a REGISTER has authenticated anyone over this connection.
  bool is_authenticated() const { return !_authenticated.empty(); }

  // Whether a whole message has ever arrived on this flow, and when the flow opened: a connection that has said
  // nothing for sip.flow_first_message_timeout is closed by the sweep.
  bool has_spoken() const { return _spoke; }
  std::chrono::steady_clock::time_point opened_at() const { return _opened_at; }

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

  // Allocated by the first stream read: a UDP flow, which the server reads for, never pays for it.
  std::vector<char> _read_buffer;
  std::string _buffer;

  bool _spoke = false;
  std::chrono::steady_clock::time_point _opened_at = std::chrono::steady_clock::now();

  // What a stream may carry before the node stops reading it: a header block with no end past this is a peer
  // that is not speaking SIP, and a body declared past this is one the node will not hold. A datagram is bounded
  // by its own size.
  static constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
  static constexpr std::size_t kMaxMessageBytes = 64 * 1024;

  // Where the search for the end of the header block resumes, so a stream fed a byte at a time is read once.
  std::size_t _scan_from = 0;

  // The stream cannot be framed any further: say why, drop what was buffered, close.
  void _overflow(const std::string& why);

  // RFC 3261 18.3: the Content-Length, when the message carries exactly one that is a number; false for garbage, a
  // value that overflows, or two of them, which a peer that meant to confuse the framing would send.
  static bool _content_length_of(const std::shared_ptr<SIPMessage>& message, std::size_t& length);

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
