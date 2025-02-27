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

Session::Session(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string nonce_secret, std::shared_ptr<Connection> connection)
    : _connection(connection), _registrar(registrar), _nonces(std::make_shared<ExpirySet<std::string>>()) {
  _logger = std::make_unique<LoggerScoped>(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), logger);
}

void Session::start() {
  auto self(this->shared_from_this());

  _logger->info("Connected");
  state = State::Normal;

  // Register callback
  _registrar->session_register(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());

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

    // Clear subscriber if exists
    if (_subscriber) {
      _registrar->subscriber_unregister(_subscriber, _contact, shared_from_this());
    }

    _logger->info("Closed");

    // Unregister Connection
    _registrar->session_unregister(_connection->transport_name() + "://" + _connection->remote_endpoint_name(), shared_from_this());

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
      if (!_subscriber && _buffer.size() > 65535) {
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
            _request->source_port = _connection->remote_endpoint().port();

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
  // std::cout << ">>>>> ";
  // _request->print();

  // Header Checking
  if (_request->header->type == SIPHeader::Type::Request) {
    // Preflight, check basic headers for a request
    if (!_request->header->contains("From") || !_request->header->contains("To") || !_request->header->contains("Call-ID") ||
        !_request->header->contains("CSeq") || !_request->header->contains("Via") || !_request->header->contains("Max-Forwards")) {
      _logger->info("[Request] - Incomplete Headers, Sending 400 Bad Request");
      _response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
      _send(400, "Bad Request");
      return;
    }
  } else {
    // Preflight, check basic headers for a response based on SIP RFCs (RFC 3261)
    // A SIP response must include: Via, From, To, Call-Id, CSeq, and Content-Length.
    if (!_request->header->contains("Via") || !_request->header->contains("From") || !_request->header->contains("To") ||
        !_request->header->contains("Call-ID") || !_request->header->contains("CSeq") || !_request->header->contains("Content-Length")) {
      _response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Content-Length)\"");
      _logger->info("[Response] - Incomplete Headers, Sending 400 Bad Response");
      _send(400, "Bad Request");
      return;
    }
  }

  // Update Existing Top Via rport if empty
  auto via = _request->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via->parameters["rport"] == "") {
    via->parameters["rport"] = std::to_string(_request->source_port);
  }

  // Are we in a call?
  auto callId = _request->header->headers_map["Call-ID"][0]->as<StringHeader>()->to_string();
  auto call = _registrar->call_get(callId);

  if (call) {
    // Add session to call if there is one
    if (!call->contains_session(shared_from_this())) {
      _logger->info("[Call " + call->id + "] Adding This Session");
      call->add_session(shared_from_this());
    }

    // Process Call State
    _process_call_state(call);

    // Forward messages to other sessions
    call->with_all_sessions_except(
        [this, call](std::shared_ptr<Session> other_session) {
          // Forward message to this session
          other_session->send(_request);
        },
        shared_from_this());

    // Free up request/response
    _request.reset();
    return;
  }

  // Create Response
  _response = std::make_shared<SIPMessage>();
  _response->header = std::make_shared<SIPHeader>();
  _response->header->type = SIPHeader::Type::Response;
  _response->body_length = 0;

  // Add From/To Headers
  // TODO: Generate proper tag
  _response->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
  _response->header->add("To", _request->header->headers_map["From"][0]);

  // Copy Request Headers
  _response->header->add("Call-ID", _request->header->headers_map["Call-ID"][0]);
  _response->header->add("CSeq", _request->header->headers_map["CSeq"][0]);
  _response->header->add("Via", _request->header->headers_map["Via"][0]);

  // Tell the client what is allowed
  _response->header->add("Allow", std::make_shared<StringHeader>("INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, INFO"));

  if (_request->header->request_method == "REGISTER") {
    _process_message_register();
  } else if (_request->header->request_method == "INVITE") {
    _process_message_invite();
  } else if (_request->header->request_method == "PUBLISH") {
    _process_message_publish();
  } else {
    _logger->debug("" + _request->header->request_method + " - Unknown Method, Sending 405 Method Not Allowed");
    _send(405, "Method Not Allowed");
  }

  // Free up request/response
  _response.reset();
  _request.reset();
}

void Session::_process_call_state(std::shared_ptr<Call> call) {
  if (_request->header->type == SIPHeader::Type::Request) {
    // Request
    if (_request->header->request_method == "INVITE") {
      auto sdp = std::make_shared<SDP>();
      if (sdp->parse(_request->body)) {
        _rewrite_sdp(sdp, _registrar->config->rtprelay_public_address, call->rtp_pair->port_a, call->rtcp_pair->port_a);
        _logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (INVITE, incall)");
        _request->body = sdp->to_string();
        // sdp->print();
      }
    }
    if (_request->header->request_method == "BYE") {
      if (call->state != Call::State::Closing) {
        _logger->debug("[Call " + call->id + "] Received BYE, Closing...");
        call->state = Call::State::Closing;
      }
    } else if (_request->header->request_method == "ACK") {
      if (call->state == Call::State::Closing) {
        _logger->debug("[Call " + call->id + "] Received ACK, Closed");
        state = State::Normal;
        _registrar->call_unregister(call->id);
        call->rtp_pair->stop();
        call->rtcp_pair->stop();
        call.reset();
        return;
      }
    }
  } else {
    // Response
    if (_request->header->response_code == 200) {
      if (call->state == Call::State::Closing) {
        _logger->info("[Call " + call->id + "] Completed");
        state = State::Normal;
        _registrar->call_unregister(call->id);
        call->rtp_pair->stop();
        call->rtcp_pair->stop();
        call.reset();
        return;
      } else if (call->state == Call::State::Ringing) {
        _logger->info("[Call " + call->id + "] Connected");
        call->state = Call::State::Connected;
      }

      // Rewrite SDP if required
      if (call->state == Call::State::Ringing || call->state == Call::State::Connected) {
        // Parse SDP
        auto sdp = std::make_shared<SDP>();
        if (sdp->parse(_request->body)) {
          // Rewrite SDP
          _rewrite_sdp(sdp, _registrar->config->rtprelay_public_address, call->rtp_pair->port_b, call->rtcp_pair->port_b);
          _logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (200)");
          _request->body = sdp->to_string();
          // sdp->print();
        }
      }
    } else if (_request->header->response_code == 100) {
      _logger->info("[Call " + call->id + "] Trying");
      call->state = Call::State::Trying;
    } else if (_request->header->response_code == 180) {
      _logger->info("[Call " + call->id + "] Ringing");
      call->state = Call::State::Ringing;
    } else if (_request->header->response_code == 603) {
      _logger->info("[Call " + call->id + "] Declined");
      call->state = Call::State::Closing;
    } else if (_request->header->response_code == 487) {
      _logger->info("[Call " + call->id + "] Request terminated");
      call->state = Call::State::Closing;
    }
  }
}

void Session::_send_auth_challenge() {
  auto nonce = _generate_nonce();
  _nonces->add(nonce, 3600);

  auto authHeader = std::make_shared<Authorization>();
  authHeader->type = "Digest";
  authHeader->fields["realm"] = _request->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value->uri->realm;
  authHeader->fields["nonce"] = nonce;
  authHeader->fields["algorithm"] = "MD5";
  authHeader->fields["stale"] = "true";
  _response->header->add("WWW-Authenticate", std::make_shared<AuthorizationHeader>(authHeader));

  _logger->info("REGISTER - Sending 401 Challenge");
  _send(401, "Unauthorized");
}

void Session::_process_message_register() {
  // The Authorization must have been sent
  if (!_request->header->contains("Authorization")) {
    _logger->debug("REGISTER - No Authorization Header, Sending 401 Unauthorized");
    _response->header->add("Reason", "No Authorization header");
    _send_auth_challenge();
    return;
  }

  // Process it and the identity
  auto incomingAuthHeader = _request->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
  auto fromIdentity = _request->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;

  // Check nonce exists
  auto nonce = incomingAuthHeader->fields["nonce"];
  if (!_nonces->contains(nonce)) {
    _logger->debug("REGISTER - Nonce not found or expired, Sending 401 Unauthorized");
    _response->header->add("Reason", "Nonce not found or expired");
    _send_auth_challenge();
    return;
  }

  // Get Subscriber
  _subscriber = _registrar->subscriber_get(fromIdentity);

  // Not Found
  if (!_subscriber) {
    _logger->debug("REGISTER - User " + fromIdentity->to_string() + " Not Found, Sending 401 Unauthorized");
    _send_auth_challenge();
    return;
  }

  // Generate H2/H3
  auto h2 = Util::md5("REGISTER:" + incomingAuthHeader->fields["uri"]);
  auto const colon = std::string(":");
  auto h3 = Util::md5(_subscriber->h1 + colon + nonce + colon + h2);

  // Check match
  if (h3 != incomingAuthHeader->fields["response"]) {
    _logger->info("REGISTER - User " + fromIdentity->to_string() + " Digest hash does not match, Sending 401 Unauthorized and Closing");
    _send_auth_challenge();
    _subscriber = nullptr;
    return;
  }

  // Authorized
  _logger->debug("REGISTER - Authorized, Sending 200 OK");
  _response->header->add("Contact", _request->header->headers_map["Contact"][0]);

  _contact = _request->header->headers_map["Contact"][0]->as<SIPIdentityHeader>()->value->uri;
  _logger->info("REGISTER - Authorized, Registering " + _subscriber->identity->to_string() + " To " + _contact->to_string());
  _registrar->subscriber_register(_subscriber, _contact, shared_from_this());

  _send(200, "OK");
}

void Session::_process_message_publish() {
  // TODO: Implement PUBLISH

  _send(200, "OK");
}

void Session::_process_message_invite() {
  if (_request->body.empty()) {
    _logger->debug("INVITE - No Body, Sending 400 Bad Request");
    _response->header->add("Reason", "Missing or zero-length body");
    _send(400, "Bad Request");
    return;
  }

  // Preflight, check basic headers
  if (!_request->header->contains("Content-Type")) {
    _logger->debug("INVITE - Incomplete Headers, Sending 400 Bad Request");
    _response->header->add("Reason", "Incomplete headers (Needs Content-Type)");
    _send(400, "Bad Request");
    return;
  }

  // Check required headers present
  if (_request->header->headers_map["Content-Type"][0]->as<StringHeader>()->to_string() != "application/sdp") {
    _logger->debug("INVITE - Incorrect MIME in Content-Type, Sending 415 Unsupported Media Type");
    _send(415, "Unsupported Media Type");
    return;
  };

  // Parse Session Description Protocol
  auto sdp = std::make_shared<SDP>();
  if (!sdp->parse(_request->body)) {
    _logger->debug("INVITE - Body Not SDP, Sending 400 Bad Request");
    _response->header->add("Reason", "Parsing application/sdp body failed");
    _send(400, "Bad Request");
    return;
  };

  // Create a new Call
  auto call = std::make_shared<Call>(_request->header->headers_map["Call-ID"][0]->as<StringHeader>()->to_string());
  call->from = _request->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;
  call->to = _request->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value;
  call->add_session(shared_from_this());

  // Create RTP/RTCP Relay Pair
  call->rtp_pair = _registrar->rtprelay_allocate();
  call->rtcp_pair = _registrar->rtprelay_allocate();
  call->rtp_pair->start();
  call->rtcp_pair->start();

  // Rewrite SDP
  _rewrite_sdp(sdp, _registrar->config->rtprelay_public_address, call->rtp_pair->port_a, call->rtcp_pair->port_a);
  _logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (INVITE)");
  _request->body = sdp->to_string();
  // sdp->print();

  // Is To: a subscriber?
  auto to_subscriber = _registrar->subscriber_get(call->to);

  // Not Found TODO:Forwarding?
  if (!to_subscriber) {
    _logger->debug("INVITE - To: " + call->to->to_string() + " Not Found, Sending 404 Not Found");
    _send(404, "Not Found");
    return;
  }
  _logger->debug("INVITE - Found To: " + call->to->to_string() + " Subscriber: " + std::to_string(to_subscriber->id));

  // Is Subscriber Online? TODO: This should query other AthenaSIP Instances if not.
  auto other_session = _registrar->subscriber_get_session(to_subscriber);

  // Not Found TODO:Forwarding?
  if (!other_session) {
    call->add_session(other_session);
    _logger->debug("INVITE - To: " + call->to->to_string() + " Session Not Found, Sending 404 Not Found");
    _send(404, "Not Found");
    return;
  }

  _logger->info("Registering Call " + call->id);
  _registrar->call_register(call->id, call);

  // Forward INVITE to other party
  other_session->send(_request);
}

void Session::_rewrite_sdp(std::shared_ptr<SDP> sdp, std::string server_address, uint16_t rtp_port, uint16_t rtcp_port) {
  sdp->connection.nettype = "IN";
  sdp->connection.addrtype = "IP4";
  sdp->connection.address = server_address;
  for (auto& media : sdp->mediaDescriptions) {
    media.description.port = rtp_port;
    if (media.hasConnection) {
      media.connection.nettype = "IN";
      media.connection.addrtype = "IP4";
      media.connection.address = server_address;
    }
    for (auto& a : media.attributes) {
      if (a.substr(0, 5) == "rtcp:") {
        a = "rtcp:" + std::to_string(rtcp_port);
      }
    }
  }
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
  // Get Server Endpoint
  auto lep = _connection->local_endpoint();
  auto server_endpoint = lep.address().to_string() + ":" + std::to_string(lep.port());

  // Add or Remove Via
  if (message->header->type == SIPHeader::Type::Request) {
    // TODO: Generate proper branch
    auto viaString = "SIP/2.0/TCP " + server_endpoint + ";branch=z9hG4bK.123456789";
    // Add Via Header for this server
    auto via = std::make_shared<ViaHeader>(viaString);
    message->header->add_start("Via", via);
  } else {
    // Remove Our Via Header
    int x = message->header->headers.size();
    message->header->remove_value("Via", [this, server_endpoint](std::shared_ptr<Header> header) { return header->as<ViaHeader>()->host == server_endpoint; });
  }

  // Add Record-Route so we stay in the dialog (ACK)
  message->header->add("Record-Route", "<sip:" + _registrar->config->rtprelay_public_address + ";transport=" + _connection->transport_name() + ";lr>");

  // Reset Length to body length
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));

  // Log Message
  _logger->info("< " + message->header->first_line());

  // cout Outgoing (DEBUG)
  // std::cout << "<<<<< ";
  // message->print();

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

}  // namespace athenasip