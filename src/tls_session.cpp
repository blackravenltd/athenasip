//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_session.h"

namespace athenasip {

TLSSession::TLSSession(std::shared_ptr<Logger> logger, std::shared_ptr<TLSServer> server,
                       std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection)
    : _connection(connection), _server(server) {
  auto rep = _connection->lowest_layer().remote_endpoint();
  remote_endpoint = rep.address().to_string() + ":" + std::to_string(rep.port());

  _logger = std::make_unique<LoggerScoped>(remote_endpoint, logger);
}

void TLSSession::close() {
  auto self(shared_from_this());

  boost::system::error_code ec;

  // Register Timeout
  if (_register_timeout) {
    _register_timeout->cancel();
    _register_timeout.reset();
  }

  // Ensure Connection Closed
  if (_connection && _connection->lowest_layer().is_open()) {
    _connection->shutdown(ec);
    _connection->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    _connection->lowest_layer().close(ec);

    _connection.reset();
    _logger->debug("Closed");

    _server->unregister_connection(shared_from_this());
  }
}

void TLSSession::write(std::string message) {
  auto self(shared_from_this());

  boost::asio::async_write(*_connection, boost::asio::buffer(message), [this, self](boost::system::error_code ec, std::size_t) {
    if (ec) {
      _logger->error("Write Error ");
      close();
    }
  });
}

void TLSSession::_start() {
  auto self(shared_from_this());

  _logger->info("Connected");
  state = State::Initial;

  // REGISTER timeout.
  _register_timeout = DelayedTask<int>::schedule(
      [this, self] {
        if (state == State::Registered) return 1;

        _logger->info("Exceeded REGISTER Timeout (5000ms)");

        write(
            "SIP/2.0 408 Request Timeout\r\nVia: SIP/2.0/TLS client.example.com;branch=z9hG4bK776asdhds\r\nFrom: <sip:user@example.com>;tag=123456\r\nTo: "
            "<sip:server@example.com>\r\nCall-ID: abc123@example.com\r\nCSeq: 1 REGISTER\r\nContent-Length: 0\r\n\r\n");

        close();
        return 0;
      },
      5000);

  _schedule_async_read();
}

void TLSSession::_schedule_async_read() {
  auto self(shared_from_this());

  _connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
    if (ec) {
      if (ec == boost::asio::error::operation_aborted || ec == boost::asio::error::eof) {
        _logger->info("Closed Connection");
      } else if (ec == boost::asio::ssl::error::stream_truncated) {
        _logger->info("Remote Terminated Connection");
      } else {
        _logger->error("Error during read: " + ec.what());
      }
      close();
    } else {
      // No Input, schedule read again and exit
      if (length == 0) {
        _schedule_async_read();
        return;
      };

      // Read complete
      _logger->debug("Read " + std::to_string(length) + " bytes");

      // Add to buffer
      _buffer.append(_read_buffer.data(), length);

      // Reject Crap (No register, more than 16k data sent)
      if (state != State::Registered && _buffer.size() > 65535) {
        _logger->info("Client did not send REGISTER request within 65535 bytes)");
        write("SIP/2.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        close();
        return;
      }

      // Process input

      if (_current_message) {
        // We're waiting for the rest of a body for an existing message
        if (_append_body()) {
          // Message body is complete, process it
          _process_message();
        } else {
          // Wait for more body
        }
      } else {
        // Look for header of a new message
        size_t pos;
        while ((pos = _buffer.find("\r\n\r\n")) != std::string::npos) {
          std::string sip_header = _buffer.substr(0, pos + 2);
          _buffer.erase(0, pos + 4);
          // Create a new SIPMessage
          _current_message = std::make_shared<SIPMessage>();
          // Get the header
          _current_message->header = std::make_shared<SIPHeader>(sip_header);
          // Get the Content-Length
          if (_current_message->header->headers.find("Content-Length") != _current_message->header->headers.end()) {
            _current_message->body_length = std::stoi(_current_message->header->headers["Content-Length"]);
          }
          // Process messages with or without bodies.
          if (_current_message->body_length == 0) {
            // Message With No Body
            _process_message();
          } else {
            // Message has a body.
            if (_append_body()) {
              // Message body is complete, process it
              _process_message();
            } else {
              // Wait for more body
            }
          }
        }
      }

      // Schedule Next Read
      _schedule_async_read();
    }
  });
}

bool TLSSession::_append_body() {
  auto to_append = std::min(_current_message->body_length - _current_message->body.size(), _buffer.size());
  _current_message->body += _buffer.substr(0, to_append);
  _buffer.erase(0, to_append);
  return (_current_message->body.size() == _current_message->body_length);
}

void TLSSession::_process_message() {
  _current_message->print();
  switch (state) {
    case State::Initial:
      _process_message_initial();
      break;
    case State::Challenged:
      _process_message_challenged();
    default:
      _logger->error("Unknown State while processing message: " + std::to_string(state));
  }
  _current_message.reset();  // replace with process
  return;
}

void TLSSession::_process_message_initial() {
  if (_current_message->header->request_method == "REGISTER") {
    // Generate Nonce and send 401 Unauthenticated

    auto reply = std::make_shared<SIPMessage>();
    reply->body_length = 0;

    // Set up Header
    auto nonce = _generate_nonce();
    AuthorizationHeader authHeader;

    authHeader.type = "Digest";
    authHeader["realm"] = "sip.athenasip.org";
    authHeader["nonce"] = nonce;
    authHeader["algorithm"] = "MD5";

    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;
    reply->header->response_code = 401;
    reply->header->response_message = "Unauthorized";
    reply->header->headers["WWW-Authenticate"] = authHeader.to_string();
    reply->header->headers["To"] = _current_message->header->headers["From"];
    reply->header->headers["From"] = "<sip:server@sip.athenasip.org>;tag=123456";
    reply->header->headers["Call-ID"] = _current_message->header->headers["Call-ID"];
    reply->header->headers["CSeq"] = "1 REGISTER";
    reply->header->headers["Via"] = _current_message->header->headers["Via"];
    reply->header->headers["Content-Length"] = "0";

    _logger->info("Initial / REGISTER - Sending 401 Challenge");
    write(reply->to_string());

    state = State::Challenged;
  }
}

void TLSSession::_process_message_challenged() {
  if (_current_message->header->request_method == "REGISTER") {

    auto reply = std::make_shared<SIPMessage>();
    reply->body_length = 0;

    auto incomingAuthHeader = AuthorizationHeader(_current_message->header->headers["Authorization"]);

    _logger->debug("Challenged / REGISTER - Checking Auth");

    // Set up Header
    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;

    _logger->debug("Challenged / REGISTER - Authorized, Sending 200 OK");
    reply->header->response_code = 200;
    reply->header->response_message = "OK";
    reply->header->headers["To"] = _current_message->header->headers["From"];
    reply->header->headers["Contact"] = _current_message->header->headers["Contact"];
    reply->header->headers["From"] = "<sip:server@sip.athenasip.org>;tag=123456";
    reply->header->headers["Call-ID"] = _current_message->header->headers["Call-ID"];
    reply->header->headers["CSeq"] = "1 REGISTER";
    reply->header->headers["Via"] = _current_message->header->headers["Via"];
    reply->header->headers["Content-Length"] = "0";

    reply->print();
    write(reply->to_string());

    state = State::Registered;
  }
}

std::string TLSSession::_generate_nonce() {
  std::array<unsigned char, 16> random_bytes;

  // Generate 128-bit (16-byte) secure random data
  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  // Get the current UNIX timestamp
  uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  // Concatenate random bytes and timestamp
  std::ostringstream raw_nonce_stream;
  raw_nonce_stream << Util::to_hex(random_bytes.data(), random_bytes.size()) << ":" << timestamp;
  std::string raw_nonce = raw_nonce_stream.str();

  // Compute HMAC-SHA256 using OpenSSL
  unsigned char hmac_result[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  HMAC(EVP_sha256(), _server->nonce_secret.c_str(), _server->nonce_secret.size(), reinterpret_cast<const unsigned char*>(raw_nonce.c_str()), raw_nonce.size(),
       hmac_result, &hmac_len);

  // Convert HMAC output to hex
  std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);

  // Return final nonce in format: "random:timestamp:hmac"
  return raw_nonce + ":" + hmac_hex;
}

}  // namespace athenasip