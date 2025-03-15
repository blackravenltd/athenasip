//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "session.h"
#include "sip_core.h"
#include "sip_message.h"

#include <iostream>

using namespace athenasip::headers;
using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

Session::Session(std::shared_ptr<Logger> logger, std::shared_ptr<SIPCore> core, std::shared_ptr<Connection> connection)
    : _connection(connection), _core(core) {
  _logger = std::make_unique<LoggerScoped>(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), logger);
}

void Session::start() {
  auto self(this->shared_from_this());

  _logger->info("Connected");
  state = State::Normal;

  // Register callback
  _core->registrar->session_register(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());

  // REGISTER timeout
  // TODO: Make rational
  // _register_timeout = DelayedTask<int>::schedule(
  //     [this, self] {
  //       if (state != State::Initial && state != State::Challenged) return 1;

  //       _logger->info("Exceeded REGISTER Timeout (5000ms)");

  //       write("SIP/2.0 408 Request Timeout\r\nVia: SIP/2.0/" + Util::to_upper(_connection->transport_name()) +
  //             " client.example.com;branch=z9hG4bK776asdhds\r\nFrom: <sip:user@example.com>;tag=123456\r\nTo: "
  //             "<sip:server@example.com>\r\nCall-Id: abc123@example.com\r\nCSeq: 1 REGISTER\r\nContent-Length: 0\r\n\r\n");

  //       close();

  //       _register_timeout.reset();
  //       return 0;
  //     },
  //     5000);

  _schedule_async_read();
}

void Session::close() {
  auto self(shared_from_this());

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
    _core->registrar->session_unregister(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());

    _connection.reset();
  }

  state = State::Closed;
}

void Session::send(std::shared_ptr<SIPMessage> message) {
  // Get Server Endpoint
  auto lep = _connection->local_endpoint();
  auto server_endpoint = lep.address().to_string() + ":" + std::to_string(lep.port());

  // Add or Remove Via
  if (message->header->type == SIPHeader::Type::Request) {
    // TODO: Generate proper branch
    auto viaString = "SIP/2.0/TCP " + server_endpoint + ";branch=z9hG4bK" + message->branch;
    // Add Via Header for this server
    auto via = std::make_shared<ViaHeader>(viaString);
    message->header->add_start("Via", via);
  } else {
    // Remove Our Via Header
    int x = message->header->headers.size();
    message->header->remove_value("Via", [this, server_endpoint](std::shared_ptr<Header> header) { return header->as<ViaHeader>()->host == server_endpoint; });
  }

  // Add Record-Route so we stay in the dialog
  message->header->add("Record-Route", "<sip:" + _core->registrar->config->rtprelay_public_address + ";transport=" + _connection->transport_name() + ";lr>");

  // Reset Length to body length
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));

  // Log Message
  _logger->info("< " + message->header->first_line());

  // cout Outgoing (DEBUG)
  // std::cout << "<<<<< ";
  // message->print();

  _schedule_async_write(message->to_string());
}

void Session::receive(std::shared_ptr<SIPMessage> message) {
    _logger->info("> " + message->header->first_line());

  // cout Incoming (DEBUG)
  // std::cout << ">>>>> ";
  // _incoming_message->print();

  _core->process_message(message);
}

void Session::_schedule_async_write(std::string message) {
  auto self(shared_from_this());

  _connection->async_write_some(boost::asio::buffer(message), [this, self](boost::system::error_code ec, std::size_t) {
    if (ec) {
      _logger->error("Write Error " + ec.to_string());
      close();
    }
  });
}

void Session::_schedule_async_read() {
  auto self(shared_from_this());

  // Are we already closed?
  if (!_connection) return;

  // Schedule Read
  _connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
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
            _incoming_message->session = shared_from_this();
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
  });
}

bool Session::_append_body() {
  // How much do we need to read?
  auto to_append = std::min(_incoming_message->body_length - _incoming_message->body.size(), _buffer.size());
  // Update current message body
  _incoming_message->body += _buffer.substr(0, to_append);
  _buffer.erase(0, to_append);
  // Return true if read complete
  return (_incoming_message->body.size() == _incoming_message->body_length);
}

}  // namespace athenasip