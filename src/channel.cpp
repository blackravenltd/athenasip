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
#include "types/authorization.h"
#include "types/sip_identity.h"
#include "types/sip_uri.h"
#include "types/subscriber.h"
#include "util.h"

using namespace athenasip::headers;
using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

Channel::Channel(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, std::shared_ptr<Connection> connection) : _connection(connection), _core(core) {
  _logger = std::make_unique<LoggerScoped>("channel " + _connection->transport_name() + "://" + _connection->remote_endpoint_name(), logger);
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
    _core->channel_register(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), self);

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
      // Shutdown and close connection
      if (_connection->is_open()) {
        _connection->shutdown();
        _connection->close();
      }

      _logger->info("Closed");

      // Unregister Connection
      _core->channel_unregister(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), self);

      _connection.reset();
    }

    state = State::Closed;
  });
}

void Channel::send(std::shared_ptr<SIPMessage> message) {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self, message]() { _send_on_strand(message); });
}

void Channel::_send_on_strand(std::shared_ptr<SIPMessage> message) {
  // close() resets _connection, and a transaction can still be holding this channel.
  if (!_connection) {
    _logger->info("Dropping " + message->header->first_line() + " - channel is closed");
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

  // Log Message
  _logger->info("< " + message->header->first_line());

  // cout Outgoing (DEBUG)
  message->print();

  _schedule_async_write(message->to_string());
}

// Called from the read handler, which already runs on the Core strand.
void Channel::receive(std::shared_ptr<SIPMessage> message) {
  _logger->info("> " + message->header->first_line());
  message->print();

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
  auto self(shared_from_this());

  if (!_connection) return;

  // The buffer has to outlive the call, so it is owned by the completion handler.
  auto buffer = std::make_shared<std::string>(std::move(message));

  _connection->async_write_some(boost::asio::buffer(*buffer), [this, self, buffer](boost::system::error_code ec, std::size_t) {
    // The completion runs on the connection's own io_context thread, so hop back.
    boost::asio::post(_core->strand(), [this, self, ec]() {
      if (ec) {
        _logger->error("Write Error " + ec.to_string());
        close();
      }
    });
  });
}

void Channel::_schedule_async_read() {
  auto self(shared_from_this());

  // Are we already closed?
  if (!_connection) return;

  // Schedule Read
  _connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
    // The completion runs on the connection's own io_context thread. Everything the
    // body touches is strand-confined, so hop back before any of it.
    boost::asio::post(_core->strand(), [this, self, ec, length]() { _on_read(ec, length); });
  });
}

void Channel::_on_read(boost::system::error_code ec, std::size_t length) {
  auto self(shared_from_this());

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

      // Process input
      if (_incoming_message) {
        // We're waiting for the rest of a body for an existing message
        if (_append_body()) {
          // Message body is complete, process it
          receive(_incoming_message);
          _incoming_message = nullptr;
        } else {
          // Wait for more body
        }
      } else {
        // Skip whitespace
        while (_buffer.size() >= 2 && _buffer.substr(0, 2) == "\r\n") _buffer.erase(0, 2);

        // Null message?
        if (_buffer.size() != 0) {
          // Look for header of a new message
          size_t pos;
          while ((pos = _buffer.find("\r\n\r\n")) != std::string::npos) {
            std::string sip_header = _buffer.substr(0, pos);
            _buffer.erase(0, pos + 4);
            // Create a new SIPMessage
            _incoming_message = std::make_shared<SIPMessage>();
            _incoming_message->channel = shared_from_this();
            _incoming_message->source_port = _connection->remote_endpoint().port();

            // Get the header
            _incoming_message->header = std::make_shared<SIPHeader>(sip_header);

            // Get the Content-Length
            if (_incoming_message->header->contains("Content-Length")) {
              _incoming_message->body_length = _incoming_message->header->headers_map["Content-Length"][0]->as<UIntHeader>()->value;
            }

            // Process messages with or without bodies.
            if (_incoming_message->body_length == 0) {
              // Message With No Body
              receive(_incoming_message);
              _incoming_message = nullptr;
            } else {
              // Message has a body.
              if (_append_body()) {
                // Message body is complete, process it
                receive(_incoming_message);
                _incoming_message = nullptr;
              } else {
                // Wait for more body
              }
            }
          }
        }
      }

      // Schedule Next Read
      if (_connection) _schedule_async_read();
    }
  }
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