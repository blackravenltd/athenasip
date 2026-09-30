//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "channel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "call.h"
#include "delayed_task.h"
#include "expiry_set.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "sdp.h"
#include "servers/connection.h"
#include "sip_header.h"
#include "sip_message.h"
#include "types/account.h"
#include "types/authorization.h"
#include "types/sip_identity.h"
#include "types/sip_uri.h"
#include "util.h"

using namespace athenasip::headers;
using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

namespace {

// The whole message as it went on the wire, with credentials taken out of it.
//
// A Digest response is a hash rather than the password, but it is replayable for as
// long as its nonce lives, and a challenge carries the nonce the next response is
// computed over. A log is a file somebody else can read, so neither goes in one. The
// line is kept rather than dropped, because knowing that a request carried credentials
// is part of reading the exchange.
std::string for_logging(const std::shared_ptr<SIPMessage>& message) {
  static const std::vector<std::string> secret = {"authorization:", "proxy-authorization:", "www-authenticate:", "proxy-authenticate:"};

  const auto wire = message->to_string();

  std::string out;
  out.reserve(wire.size());

  std::size_t at = 0;
  while (at < wire.size()) {
    auto end = wire.find("\r\n", at);
    if (end == std::string::npos) end = wire.size();

    const auto line = wire.substr(at, end - at);
    const auto lowered = Util::to_lower(line);

    bool redacted = false;
    for (const auto& name : secret) {
      if (lowered.rfind(name, 0) == 0) {
        out += line.substr(0, name.size() - 1) + ": <redacted>";
        redacted = true;
        break;
      }
    }

    if (!redacted) out += line;

    if (end == wire.size()) break;

    out += "\r\n";
    at = end + 2;
  }

  return out;
}

}  // namespace

Channel::Channel(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, std::shared_ptr<Connection> connection) : _connection(connection), _core(core) {
  _flow_id = Core::channel_key(_connection->transport_name(), _connection->remote_endpoint_name());
  _flow_token = Util::generate_random_string("f", 20);
  _logger = std::make_unique<LoggerScoped>("channel " + _flow_id, logger);
}

void Channel::start() {
  auto self(this->shared_from_this());

  // Servers call this from their own thread. Everything a channel touches belongs to
  // the Core strand, so hand over immediately. dispatch, not post: a caller already on
  // the strand runs inline.
  boost::asio::dispatch(_core->strand(), [this, self]() {
    _logger->info("Connected");
    state = State::Normal;

    // Register callback
    _core->channel_register(_flow_id, self);

    // REGISTER timeout
    // TODO: Make rational

    _schedule_async_read();
  });
}

void Channel::close() {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self]() {
    if (state == State::Closing || state == State::Closed) return;

    state = State::Closing;

    // Ensure Connection Closed
    if (_connection) {
      // The socket belongs to its server's thread, which may be inside it right now
      // finishing a read the far end has just ended. Tearing it down from here would be
      // a second thread in a stream that is not safe for one, so hand it over and let
      // the strand get on with the registry.
      boost::asio::dispatch(_connection->executor(), [connection = _connection]() {
        if (connection->is_open()) {
          connection->shutdown();
          connection->close();
        }
      });

      _logger->info("Closed");

      // Unregister Connection
      _core->channel_unregister(_flow_id, self);

      _connection.reset();
    }

    state = State::Closed;
  });
}

void Channel::write(std::string message) {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self, message = std::move(message)]() mutable { _schedule_async_write(std::move(message)); });
}

void Channel::send(std::shared_ptr<SIPMessage> message) {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self, message]() { _send_on_strand(message); });
}

void Channel::_send_on_strand(std::shared_ptr<SIPMessage> message) {
  // close() resets _connection, and a transaction can still be holding this channel.
  if (!_connection) {
    _logger->info("Dropping " + message->header->summary() + " - channel is closed");
    return;
  }

  // Via belongs to the layers above: the proxy adds its own on forward (RFC 3261 16.6
  // step 8) and strips it from responses (16.7 step 3). The transport's only remaining
  // interest is that nothing leaves with a branchless Via, because a branch is what names
  // the transaction the answer has to find (8.1.1.7).
  if (message->header->type == SIPHeader::Type::Request && message->header->contains("Via") && !message->header->headers_map["Via"].empty()) {
    auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();

    if (via != nullptr && via->parameters["branch"].empty()) {
      if (message->branch.empty()) message->branch = Util::generate_random_string("z9hG4bK", 16);
      via->parameters["branch"] = message->branch;
    } else if (via != nullptr) {
      message->branch = via->parameters["branch"];
    }
  }

  // Reset Length to body length
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));

  _logger->info("< " + message->header->summary());
  if (_core->config->sip_log_messages) _logger->debug("< " + for_logging(message));

  _schedule_async_write(message->to_string());
}

// Called from the read handler, which already runs on the Core strand.
void Channel::receive(std::shared_ptr<SIPMessage> message) {
  _logger->info("> " + message->header->summary());
  if (_core->config->sip_log_messages) _logger->debug("> " + for_logging(message));

  message->channel = shared_from_this();

  _stamp_via(message);

  _core->process_message(message);
}

// RFC 3261 18.2.1 and RFC 3581 section 4. A client behind NAT sees a different address
// and port from the one it put in its Via, so the response would go nowhere. received
// records where the request actually came from, and rport, when the client asked for it
// by sending the parameter empty, records the port as well. This is a transport fact, so
// the transport is what writes it.
void Channel::_stamp_via(const std::shared_ptr<SIPMessage>& message) {
  if (!_connection) return;
  if (message->header->type != SIPHeader::Type::Request) return;
  if (!message->header->contains("Via") || message->header->headers_map["Via"].empty()) return;

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via == nullptr) return;

  const auto remote = _connection->remote_endpoint();
  const auto source = remote.address().to_string();

  // The Via host may carry a port, so compare on the address alone.
  const auto colon = via->host.rfind(':');
  const auto sent_by = colon == std::string::npos ? via->host : via->host.substr(0, colon);

  if (sent_by != source) via->parameters["received"] = source;

  // Present and empty means "tell me the port"; present with a value is not ours to
  // overwrite, and absent means the client does not want it.
  auto rport = via->parameters.find("rport");
  if (rport != via->parameters.end() && rport->second.empty()) rport->second = std::to_string(remote.port());
}

void Channel::_schedule_async_write(std::string message) {
  if (!_connection) return;

  // Sending counts as much as receiving. A node answering a retransmission, or forwarding
  // into a flow, is using it even if the far end has gone quiet.
  touch();

  // The buffer has to outlive the write, so the queue owns it.
  _write_queue.push_back(std::make_shared<std::string>(std::move(message)));

  // Already writing: the completion will pick this up. Starting a second write here is
  // what puts two of them on one socket.
  if (_writing) return;

  _write_next();
}

void Channel::_write_next() {
  if (!_connection || _write_queue.empty()) {
    _writing = false;
    return;
  }

  _writing = true;

  auto self(shared_from_this());
  auto buffer = _write_queue.front();
  const auto offset = _write_offset;

  // Starting the write is touching the stream, so it happens where the stream lives.
  boost::asio::dispatch(_connection->executor(), [this, self, buffer, offset, connection = _connection]() {
    connection->async_write_some(boost::asio::buffer(buffer->data() + offset, buffer->size() - offset),
                                 [this, self, buffer](boost::system::error_code ec, std::size_t length) {
                                   // The completion runs on the connection's own io_context thread, so hop back.
                                   boost::asio::post(_core->strand(), [this, self, ec, length]() { _on_write(ec, length); });
                                 });
  });
}

void Channel::_on_write(boost::system::error_code ec, std::size_t length) {
  if (ec) {
    _logger->error("Write Error " + ec.to_string());

    // Nothing queued behind a failed write is going anywhere: the channel is closing.
    _write_queue.clear();
    _write_offset = 0;
    _writing = false;

    close();
    return;
  }

  // async_write_some is not obliged to take the whole buffer, and a short write that
  // nobody resumes is a truncated SIP message on the wire.
  _write_offset += length;

  if (!_write_queue.empty() && _write_offset >= _write_queue.front()->size()) {
    _write_queue.pop_front();
    _write_offset = 0;
  }

  _write_next();
}

void Channel::_schedule_async_read() {
  auto self(shared_from_this());

  // Are we already closed?
  if (!_connection) return;

  // Schedule Read, on the connection's own executor: only one read is outstanding at a
  // time, so the buffer is this channel's until the completion hands it back.
  boost::asio::dispatch(_connection->executor(), [this, self, connection = _connection]() {
    connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
      // The completion runs on the connection's own io_context thread. Everything the
      // body touches is strand-confined, so hop back before any of it.
      boost::asio::post(_core->strand(), [this, self, ec, length]() { _on_read(ec, length); });
    });
  });
}

void Channel::touch() { _last_activity = _core->now(); }

void Channel::_on_read(boost::system::error_code ec, std::size_t length) {
  auto self(shared_from_this());

  // Before anything is decided about the bytes: a flow that carried something is a flow
  // in use, whatever the something turns out to be.
  if (!ec && length > 0) touch();

  {
    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        // Normal (We closed the connection)
      } else if (ec == boost::asio::error::eof) {
        _logger->info("Remote Disconnected");
      } else {
        _logger->error("Read Error (" + ec.what() + ")");
      }
      close();
    } else {
      // No Input, schedule read again and exit
      if (length == 0) {
        if (_connection) _schedule_async_read();
        return;
      };

      // Add to buffer
      _buffer.append(_read_buffer.data(), length);

      // Nothing below may throw out of here. This is the read handler, so an exception
      // from a message off the network unwinds through io_context::run() and takes the
      // process with it: one malformed datagram from anybody would be the whole node.
      try {
        _frame();
      } catch (const std::exception& e) {
        _logger->error(std::string("Dropping a message that could not be handled - ") + e.what());
        _buffer.clear();
        _incoming_message = nullptr;
      }

      // Schedule Next Read
      if (_connection) _schedule_async_read();
    }
  }
}

// RFC 3261 18.3. A stream has no message boundaries, so Content-Length is what says
// where a body ends and a message may arrive in as many reads as the network likes. A
// datagram is one message on its own and nothing carries over between them, which is a
// different rule and not a special case of the same one.
void Channel::_frame() {
  if (_connection && !_connection->is_reliable()) return _frame_datagram();

  _frame_stream();
}

void Channel::_frame_stream() {
  // Still short of a body promised by a header already read.
  if (_incoming_message) {
    if (!_append_body()) return;

    receive(_incoming_message);
    _incoming_message = nullptr;
    return;
  }

  // A CRLF between messages is a keep-alive, not a message (RFC 5626 section 4.4.1).
  while (_buffer.size() >= 2 && _buffer.compare(0, 2, "\r\n") == 0) _buffer.erase(0, 2);

  // As many whole messages as the buffer holds. A stream may deliver several in one
  // read and half of one in the next, and both have to come out right.
  std::size_t split;

  while ((split = _buffer.find("\r\n\r\n")) != std::string::npos) {
    auto message = std::make_shared<SIPMessage>();
    message->channel = shared_from_this();
    message->source_port = _connection->remote_endpoint().port();
    message->header = std::make_shared<SIPHeader>(_buffer.substr(0, split));

    _buffer.erase(0, split + 4);

    // RFC 3261 18.3: on a stream transport Content-Length is the only thing that says
    // where the body ends. Absent, it is zero, which is what a request with no body
    // carries.
    if (message->header->contains("Content-Length")) {
      auto length = message->header->headers_map["Content-Length"][0]->as<UIntHeader>();
      if (length != nullptr) message->body_length = length->value;
    }

    _incoming_message = message;

    if (!_append_body()) return;

    receive(_incoming_message);
    _incoming_message = nullptr;

    while (_buffer.size() >= 2 && _buffer.compare(0, 2, "\r\n") == 0) _buffer.erase(0, 2);
  }
}

// One datagram is one message, whole or not at all. A datagram that is not a whole
// message is discarded and the next one starts clean: leaving a half-message behind
// would let one sender's truncated request swallow the next request to arrive on a flow
// they share, and on UDP every peer at one address and port shares a flow.
void Channel::_frame_datagram() {
  std::string datagram;
  datagram.swap(_buffer);
  _incoming_message = nullptr;

  // A datagram of CRLFs is a keep-alive, not a message (RFC 5626 section 4.4.1).
  std::size_t at = 0;
  while (datagram.size() - at >= 2 && datagram.compare(at, 2, "\r\n") == 0) at += 2;
  if (at >= datagram.size()) return;

  const auto split = datagram.find("\r\n\r\n", at);

  if (split == std::string::npos) {
    _logger->warn("Datagram with no blank line after its headers, discarded");
    return;
  }

  auto message = std::make_shared<SIPMessage>();
  message->channel = shared_from_this();
  message->source_port = _connection->remote_endpoint().port();
  message->header = std::make_shared<SIPHeader>(datagram.substr(at, split - at));

  std::size_t declared = 0;

  if (message->header->contains("Content-Length")) {
    auto length = message->header->headers_map["Content-Length"][0]->as<UIntHeader>();
    if (length != nullptr) declared = length->value;
  }

  const auto carried = datagram.size() - (split + 4);

  // "If the message has a Content-Length header field value that is greater than the
  // size of the body, the message MUST be discarded." There is no later packet that
  // completes a datagram, so waiting for one is waiting for something that cannot come.
  if (declared > carried) {
    _logger->warn("Datagram claims " + std::to_string(declared) + " bytes of body and carries " + std::to_string(carried) + ", discarded");
    return;
  }

  // "If it is less than the size of the body, the body is truncated to that length."
  // The bytes after it are not a second message: a datagram carries one.
  message->body = datagram.substr(split + 4, declared);
  message->body_length = static_cast<unsigned int>(declared);

  receive(message);
}

bool Channel::_append_body() {
  // How much do we need to read?
  auto to_append = std::min(_incoming_message->body_length - _incoming_message->body.size(), _buffer.size());
  // Update current message body
  _incoming_message->body += _buffer.substr(0, to_append);
  _buffer.erase(0, to_append);
  // Return true if read complete
  return (_incoming_message->body.size() == _incoming_message->body_length);
}

}  // namespace athenasip