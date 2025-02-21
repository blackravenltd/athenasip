//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "session.h"

#include <iostream>

using namespace athenasip::headers;
using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

Session::Session(std::shared_ptr<Logger> logger, std::string nonce_secret, std::shared_ptr<Connection> connection)
    : _connection(connection), _nonces(std::make_shared<ExpirySet<std::string>>()) {
  _logger = std::make_unique<LoggerScoped>(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), logger);
}

void Session::start() {
  auto self(shared_from_this());

  _logger->info("Connected");
  state = State::Initial;

  // Register callback
  if (_on_start) _on_start(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());

  // REGISTER timeout
  _register_timeout = DelayedTask<int>::schedule(
      [this, self] {
        if (state != State::Initial && state != State::Challenged) return 1;

        _logger->info("Exceeded REGISTER Timeout (5000ms)");

        write("SIP/2.0 408 Request Timeout\r\nVia: SIP/2.0/" + Util::to_upper(_connection->transport_name()) +
              " client.example.com;branch=z9hG4bK776asdhds\r\nFrom: <sip:user@example.com>;tag=123456\r\nTo: "
              "<sip:server@example.com>\r\nCall-ID: abc123@example.com\r\nCSeq: 1 REGISTER\r\nContent-Length: 0\r\n\r\n");

        close();

        _register_timeout.reset();
        return 0;
      },
      5000);

  _schedule_async_read();
}

void Session::close() {
  auto self(shared_from_this());

  if (state == State::Closing || state == State::Closed) return;

  state = State::Closing;

  // Cancel Register Timeout
  if (_register_timeout) {
    _register_timeout->cancel();
    _register_timeout.reset();
  }

  // Ensure Connection Closed
  if (_connection) {
    // Shutdown and close connection
    if (_connection->is_open()) {
      _connection->shutdown();
      _connection->close();
    }

    if (_on_unregister_location && _subscriber) {
      _on_unregister_location(_subscriber, _contact, shared_from_this());
      _on_unregister_location = nullptr;
    }

    _logger->info("Closed");

    // OnClose callback
    if (_on_close) {
      _on_close(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());
      _on_close = nullptr;
    }

    _connection.reset();
  }

  state = State::Closed;
}

void Session::write(std::string message) {
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

      // Reject Crap (No register, more than 64k data sent)
      if (state != State::Registered && _buffer.size() > 65535) {
        _logger->info("Client did not send REGISTER request within 65535 bytes)");
        write("SIP/2.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        close();
        return;
      }

      // Process input
      if (_request) {
        // We're waiting for the rest of a body for an existing message
        if (_append_body()) {
          // Message body is complete, process it
          _process_message();
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
            _request = std::make_shared<SIPMessage>();

            // Get the header
            _request->header = std::make_shared<SIPHeader>(sip_header);

            // Get the Content-Length
            if (_request->header->contains("Content-Length")) {
              _request->body_length = _request->header->headers_map["Content-Length"][0]->as<UIntHeader>()->value;
            }

            // Process messages with or without bodies.
            if (_request->body_length == 0) {
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
      }

      // Schedule Next Read
      if (_connection) _schedule_async_read();
    }
  });
}

bool Session::_append_body() {
  // How much do we need to read?
  auto to_append = std::min(_request->body_length - _request->body.size(), _buffer.size());
  // Update current message body
  _request->body += _buffer.substr(0, to_append);
  _buffer.erase(0, to_append);
  // Return true if read complete
  return (_request->body.size() == _request->body_length);
}

void Session::_process_message() {
  // Log Message
  _logger->info("> " + _request->header->first_line());

  // cout Incoming (DEBUG)
  std::cout << ">>>>> ";
  _request->print();

  // Create Response
  _response = std::make_shared<SIPMessage>();
  _response->header = std::make_shared<SIPHeader>();
  _response->header->type = SIPHeader::Type::Response;
  _response->body_length = 0;

  if (_request->header->type == SIPHeader::Type::Request) {
    // Preflight, check basic headers for a request
    if (!_request->header->contains("From") || !_request->header->contains("To") || !_request->header->contains("Call-ID") ||
        !_request->header->contains("CSeq") || !_request->header->contains("Via") || !_request->header->contains("Max-Forwards")) {
      _logger->info("[Request] - Incomplete Headers, Sending 400 Bad Request");
      _send(400, "Bad Request");
      return;
    }
  } else {
    // Preflight, check basic headers for a response based on SIP RFCs (RFC 3261)
    // A SIP response must include: Via, From, To, Call-ID, CSeq, and Content-Length.
    if (!_request->header->contains("Via") || !_request->header->contains("From") || !_request->header->contains("To") ||
        !_request->header->contains("Call-ID") || !_request->header->contains("CSeq") || !_request->header->contains("Content-Length")) {
      _logger->info("[Response] - Incomplete Headers, Sending 400 Bad Response");
      return;
    }
  }

  // Add From/To Headers
  _response->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
  _response->header->add("To", _request->header->headers_map["From"][0]);

  // Copy Request Headers
  _response->header->add("Call-Id", _request->header->headers_map["Call-ID"][0]);
  _response->header->add("CSeq", _request->header->headers_map["CSeq"][0]);
  _response->header->add("Via", _request->header->headers_map["Via"][0]);

  // Tell the client what is allowed
  _response->header->add("Allow", std::make_shared<StringHeader>("INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, INFO"));

  // Process according to state
  switch (state) {
    case State::Initial:
      _process_message_initial();
      break;
    case State::Challenged:
      _process_message_challenged();
      break;
    case State::Registered:
      _process_message_registered();
      break;
    case State::InCall:
      _process_message_incall();
      break;
    case State::Bye:
      _process_message_bye();
      break;
    default:
      _logger->error("Unknown State while processing message: " + std::to_string(state));
  }

  // Free up request/response
  _request.reset();
  _response.reset();
  return;
}

void Session::_process_message_initial() {
  if (_request->header->request_method == "REGISTER") {
    // Generate and save nonce
    auto nonce = _generate_nonce();
    _nonces->add(nonce, 3600);

    auto authHeader = std::make_shared<Authorization>();
    authHeader->type = "Digest";
    authHeader->fields["realm"] = "sip.athenasip.org";
    authHeader->fields["nonce"] = nonce;
    authHeader->fields["algorithm"] = "MD5";
    _response->header->add("WWW-Authenticate", std::make_shared<AuthorizationHeader>(authHeader));

    _logger->info("Initial / REGISTER - Sending 401 Challenge");
    state = State::Challenged;

    _send(401, "Unauthorized");
  }
}

void Session::_process_message_challenged() {
  if (_request->header->request_method == "REGISTER") {
    // The Authorization must have been sent
    if (!_request->header->contains("Authorization")) {
      _logger->info("Challenged / REGISTER - No Authorization Header, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Process it and the identity
    auto incomingAuthHeader = _request->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
    auto fromIdentity = _request->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;

    // Check nonce exists
    auto nonce = incomingAuthHeader->fields["nonce"];
    if (!_nonces->contains(nonce)) {
      _logger->info("Challenged / REGISTER - Nonce not found or expired, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Get Subscriber
    _subscriber = _on_authenticate(fromIdentity, shared_from_this());

    // Not Found
    if (!_subscriber) {
      _logger->info("Challenged / REGISTER - User " + fromIdentity->to_string() + " Not Found, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Generate H2/H3
    auto h2 = Util::md5("REGISTER:" + incomingAuthHeader->fields["uri"]);
    auto const colon = std::string(":");
    auto h3 = Util::md5(_subscriber->h1 + colon + nonce + colon + h2);

    // Check match
    if (h3 != incomingAuthHeader->fields["response"]) {
      _logger->info("Challenged / REGISTER - User " + fromIdentity->to_string() + " Digest hash does not match, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      _subscriber = nullptr;
      return;
    }

    // Authorized
    _logger->debug("Challenged / REGISTER - Authorized, Sending 200 OK");
    _response->header->add("Contact", _request->header->headers_map["Contact"][0]);

    _contact = _request->header->headers_map["Contact"][0]->as<SIPIdentityHeader>()->value->uri;
    _logger->info("Challenged / REGISTER - Authorized, Registering " + _subscriber->identity->to_string() + " To " + _contact->to_string());
    _on_register_location(_subscriber, _contact, shared_from_this());

    state = State::Registered;
    _send(200, "OK");
  }
}

void Session::_process_message_registered() {
  if (_request->header->request_method == "INVITE") {
    _process_message_invite();
    return;
  }

  _logger->debug("Registered / " + _request->header->request_method + " - Unknown Method, Sending 405 Method Not Allowed");
  _send(405, "Method Not Allowed");
}

void Session::_process_message_invite() {
  if (_request->body.empty()) {
    _logger->debug("Registered / INVITE - No Body, Sending 400 Bad Request");
    _send(400, "Bad Request");
    return;
  }

  // Preflight, check basic headers
  if (!_request->header->contains("Content-Type")) {
    _logger->debug("Registered / INVITE - Incomplete Headers, Sending 400 Bad Request");
    _send(400, "Bad Request");
    return;
  }

  // Check required headers present
  if (_request->header->headers_map["Content-Type"][0]->as<StringHeader>()->to_string() != "application/sdp") {
    _logger->debug("Registered / INVITE - Incorrect MIME in Content-Type, Sending 415 Unsupported Media Type");
    _send(415, "Unsupported Media Type");
    return;
  };

  // Parse Session Description Protocol
  auto sdp = std::make_shared<SDP>();
  if (!sdp->parse(_request->body)) {
    _logger->debug("Registered / INVITE - Body Not SDP, Sending 400 Bad Request");
    _send(400, "Bad Request");
    return;
  };

  auto toIdentity = _request->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value;

  // Is To: a subscriber?
  auto to_subscriber = _on_authenticate(toIdentity, shared_from_this());

  // Not Found TODO:Forwarding?
  if (!to_subscriber) {
    _logger->debug("Registered / INVITE - To: " + toIdentity->to_string() + " Not Found, Sending 404 Not Found");
    _send(404, "Not Found");
    return;
  }

  _logger->debug("Registered / INVITE - Found To: " + toIdentity->to_string() + " Subscriber: " + std::to_string(to_subscriber->id));

  // Is Subscriber Online? TODO: This should query other AthenaSIP Instances if not.
  auto to_session = _on_get_subscriber_session(to_subscriber);

  // Not Found TODO:Forwarding?
  if (!to_session) {
    _logger->debug("Registered / INVITE - To: " + toIdentity->to_string() + " Session Not Found, Sending 404 Not Found");
    _send(404, "Not Found");
    return;
  }

  // Send Trying
  // _send(100,"Trying");

  if (to_session->state != State::Registered) {
    _logger->debug("Registered / INVITE - To: " + toIdentity->to_string() + " Session State not State::Registered, Sending 486 Busy Here");
    _send(486, "Busy Here");
    return;
  }

  // Set States
  state = State::InCall;
  to_session->state = State::InCall;

  // Set Other session
  other_session = to_session;
  to_session->other_session = shared_from_this();

  // Blank Params/Headers
  _request->header->clear("Allow");
  _request->header->add("Allow", std::make_shared<StringHeader>("INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, INFO"));

  // Forward INVITE to other party
  other_session->send(_request);
}

void Session::_process_message_incall() {
  // Dumbly relay messages unless "BYE"
  other_session->send(_request);

  if (_request->header->type == SIPHeader::Type::Request && _request->header->request_method == "BYE") {
    state = State::Bye;
    other_session->state = State::Bye;
  }
}

void Session::_process_message_bye() {
  // Relay one message then close.
  other_session->send(_request);

  state = State::Registered;
  other_session->state = State::Registered;

  other_session->other_session = nullptr;
  other_session = nullptr;
}

void Session::_send_close(uint16_t code, std::string message) {
  _send(code, message);
  close();
}

void Session::_send(uint16_t code, std::string message) {
  _response->header->response_code = code;
  _response->header->response_message = message;
  send(_response);
}

void Session::send(std::shared_ptr<SIPMessage> message) {
  // Reset Length to body length
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));

  // Log Message
  _logger->info("< " + message->header->first_line());

  // cout Outgoing (DEBUG)
  std::cout << "<<<<< ";
  message->print();

  write(message->to_string());
}

std::string Session::_generate_nonce() const {
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

  HMAC(EVP_sha256(), _nonce_secret.c_str(), _nonce_secret.size(), reinterpret_cast<const unsigned char*>(raw_nonce.c_str()), raw_nonce.size(), hmac_result,
       &hmac_len);

  // Convert HMAC output to hex
  std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);

  // Return final nonce in format: "random:timestamp:hmac"
  return raw_nonce + ":" + hmac_hex;
}

void Session::on_start(StartCloseFn callback) { _on_start = callback; }
void Session::on_close(StartCloseFn callback) { _on_close = callback; }
void Session::on_authenticate(AuthenticateFn callback) { _on_authenticate = callback; }
void Session::on_register_location(RegisterLocationFn callback) { _on_register_location = callback; }
void Session::on_unregister_location(RegisterLocationFn callback) { _on_unregister_location = callback; }
void Session::on_get_subscriber_session(GetSubscriberSessionFn callback) { _on_get_subscriber_session = callback; }

}  // namespace athenasip