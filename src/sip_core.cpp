//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_core.h"

namespace athenasip {

SIPCore::SIPCore() {}
SIPCore::SIPCore(std::shared_ptr<Logger> _logger, std::shared_ptr<Version> _version, std::shared_ptr<Config> _config,
                 std::shared_ptr<athenasip::databases::DB> _db, std::shared_ptr<Registrar> registrar)
    : logger(std::make_shared<LoggerScoped>("core", _logger)), version(_version), config(_config), db(_db), registrar(registrar) {}

void SIPCore::process_message(std::shared_ptr<SIPMessage> message) {
  auto response = std::make_shared<SIPMessage>();

  // Header Checking
  if (message->header->type == SIPHeader::Type::Request) {
    // Preflight, check basic headers for a request
    if (!message->header->contains("From") || !message->header->contains("To") || !message->header->contains("Call-ID") || !message->header->contains("CSeq") ||
        !message->header->contains("Via") || !message->header->contains("Max-Forwards")) {
      logger->info("[Request] - Incomplete Headers, Sending 400 Bad Request");
      response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
      _send(message, 400, "Bad Request");
      return;
    }
  } else {
    // Preflight, check basic headers for a response based on SIP RFCs (RFC 3261)
    // A SIP response must include: Via, From, To, Call-Id, CSeq, and Content-Length.
    if (!message->header->contains("Via") || !message->header->contains("From") || !message->header->contains("To") || !message->header->contains("Call-ID") ||
        !message->header->contains("CSeq") || !message->header->contains("Content-Length")) {
      response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Content-Length)\"");
      logger->info("[Response] - Incomplete Headers, Sending 400 Bad Response");
      _send(message, 400, "Bad Request");
      return;
    }
  }

  // Update Existing Top Via rport if empty
  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via->parameters["rport"] == "") {
    via->parameters["rport"] = std::to_string(message->source_port);
  }

  // Are we in a call?
  auto callId = message->header->headers_map["Call-ID"][0]->as<StringHeader>()->to_string();
  auto call = registrar->call_get(callId);

  if (call) {
    // Add Session to call if there is one
    if (!call->contains_session(message->session)) {
      logger->info("[Call " + call->id + "] Adding This Session");
      call->add_session(message->session);
    }

    // Process Call State
    _process_call_state(message, call);

    // Forward messages to other SIPCores
    call->with_all_sessions_except(
        [this, message, call](std::shared_ptr<Session> other_session) {
          // Forward message to this SIPCore
          other_session->send(message);
        },
        message->session);

    // Free up request/response
    message.reset();
    return;
  }

  if (message->header->request_method == "REGISTER") {
    _process_message_register(message);
  } else if (message->header->request_method == "INVITE") {
    _process_message_invite(message);
  } else if (message->header->request_method == "PUBLISH") {
    _process_message_publish(message);
  } else {
    logger->debug("" + message->header->request_method + " - Unknown Method, Sending 405 Method Not Allowed");
    _send(message, 405, "Method Not Allowed");
  }

  // Free up request/response
  response.reset();
  message.reset();
}

void SIPCore::_process_call_state(std::shared_ptr<SIPMessage> message, std::shared_ptr<Call> call) {
  if (message->header->type == SIPHeader::Type::Request) {
    // Request
    if (message->header->request_method == "INVITE") {
      auto sdp = std::make_shared<SDP>();
      if (sdp->parse(message->body)) {
        _rewrite_sdp(sdp, config->rtprelay_public_address, call->rtp_set->port, call->rtcp_set->port);
        logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (INVITE, incall)");
        message->body = sdp->to_string();
        // sdp->print();
      }
    }
    if (message->header->request_method == "BYE") {
      if (call->state != Call::State::Closing) {
        logger->debug("[Call " + call->id + "] Received BYE, Closing...");
        call->state = Call::State::Closing;
      }
    } else if (message->header->request_method == "ACK") {
      if (call->state == Call::State::Closing) {
        logger->debug("[Call " + call->id + "] Received ACK, Closed");
        registrar->call_unregister(call->id);
        call->rtp_set->stop();
        call->rtcp_set->stop();
        call.reset();
        return;
      }
    }
  } else {
    // Response
    if (message->header->response_code == 200) {
      if (call->state == Call::State::Closing) {
        logger->info("[Call " + call->id + "] Completed");
        registrar->call_unregister(call->id);
        call->rtp_set->stop();
        call->rtcp_set->stop();
        call.reset();
        return;
      } else if (call->state == Call::State::Ringing) {
        logger->info("[Call " + call->id + "] Connected");
        call->state = Call::State::Connected;
      }

      // Rewrite SDP if required
      if (call->state == Call::State::Ringing || call->state == Call::State::Connected) {
        // Parse SDP
        auto sdp = std::make_shared<SDP>();
        if (sdp->parse(message->body)) {
          // Rewrite SDP
          _rewrite_sdp(sdp, registrar->config->rtprelay_public_address, call->rtp_set->port, call->rtcp_set->port);
          logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (200)");
          message->body = sdp->to_string();
          // sdp->print();
        }
      }
    } else if (message->header->response_code == 100) {
      logger->info("[Call " + call->id + "] Trying");
      call->state = Call::State::Trying;
    } else if (message->header->response_code == 180) {
      logger->info("[Call " + call->id + "] Ringing");
      call->state = Call::State::Ringing;
    } else if (message->header->response_code == 603) {
      logger->info("[Call " + call->id + "] Declined");
      call->state = Call::State::Closing;
    } else if (message->header->response_code == 487) {
      logger->info("[Call " + call->id + "] Request terminated");
      call->state = Call::State::Closing;
    }
  }
}

void SIPCore::_send_auth_challenge(std::shared_ptr<SIPMessage> message) {
  auto nonce = registrar->nonce_get();

  auto authHeader = std::make_shared<Authorization>();
  authHeader->type = "Digest";
  authHeader->fields["realm"] = message->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value->uri->realm;
  authHeader->fields["nonce"] = nonce;
  authHeader->fields["algorithm"] = "MD5";
  authHeader->fields["stale"] = "true";

  auto response = message->generate_response();
  response->header->add("WWW-Authenticate", std::make_shared<AuthorizationHeader>(authHeader));

  logger->info("REGISTER - Sending 401 Challenge");
  _send(response, 401, "Unauthorized");
}

void SIPCore::_process_message_register(std::shared_ptr<SIPMessage> message) {
  auto response = message->generate_response();

  // The Authorization must have been sent
  if (!message->header->contains("Authorization")) {
    logger->debug("REGISTER - No Authorization Header, Sending 401 Unauthorized");
    response->header->add("Reason", "No Authorization header");
    _send_auth_challenge(response);
    return;
  }

  // Process it and the identity
  auto incomingAuthHeader = message->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
  auto fromIdentity = message->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;

  // Check nonce exists
  auto nonce = incomingAuthHeader->fields["nonce"];
  if (!registrar->nonce_check(nonce)) {
    logger->debug("REGISTER - Nonce not found or expired, Sending 401 Unauthorized");
    response->header->add("Reason", "Nonce not found or expired");
    _send_auth_challenge(response);
    return;
  }

  // Get Subscriber
  message->subscriber = registrar->subscriber_get(fromIdentity);

  // Not Found
  if (!message->subscriber) {
    logger->debug("REGISTER - User " + fromIdentity->to_string() + " Not Found, Sending 401 Unauthorized");
    _send_auth_challenge(message);
    return;
  }

  // Generate H2/H3
  auto h2 = Util::md5("REGISTER:" + incomingAuthHeader->fields["uri"]);
  auto const colon = std::string(":");
  auto h3 = Util::md5(message->subscriber->h1 + colon + nonce + colon + h2);

  // Check match
  if (h3 != incomingAuthHeader->fields["response"]) {
    logger->info("REGISTER - User " + fromIdentity->to_string() + " Digest hash does not match, Sending 401 Unauthorized and Closing");
    _send_auth_challenge(message);
    message->subscriber = nullptr;
    return;
  }

  // Authorized
  response->header->add("Contact", message->header->headers_map["Contact"][0]);
  message->contact = message->header->headers_map["Contact"][0]->as<SIPIdentityHeader>()->value->uri;
  logger->info("REGISTER - Authorized, Registering " + message->subscriber->identity->to_string() + " To " + message->contact->to_string());
  registrar->subscriber_register(message->subscriber, message->contact, message->session);

  _send(response, 200, "OK");
}

void SIPCore::_process_message_publish(std::shared_ptr<SIPMessage> message) {
  auto response = message->generate_response();

  _send(response, 200, "OK");
}

void SIPCore::_process_message_invite(std::shared_ptr<SIPMessage> message) {
  auto response = message->generate_response();

  if (message->body.empty()) {
    logger->debug("INVITE - No Body, Sending 400 Bad Request");
    response->header->add("Reason", "Missing or zero-length body");
    _send(message, 400, "Bad Request");
    return;
  }

  // Preflight, check basic headers
  if (!message->header->contains("Content-Type")) {
    logger->debug("INVITE - Incomplete Headers, Sending 400 Bad Request");
    response->header->add("Reason", "Incomplete headers (Needs Content-Type)");
    _send(message, 400, "Bad Request");
    return;
  }

  // Check required headers present
  if (message->header->headers_map["Content-Type"][0]->as<StringHeader>()->to_string() != "application/sdp") {
    logger->debug("INVITE - Incorrect MIME in Content-Type, Sending 415 Unsupported Media Type");
    _send(message, 415, "Unsupported Media Type");
    return;
  };

  // Parse SIPCore Description Protocol
  auto sdp = std::make_shared<SDP>();
  if (!sdp->parse(message->body)) {
    logger->debug("INVITE - Body Not SDP, Sending 400 Bad Request");
    response->header->add("Reason", "Parsing application/sdp body failed");
    _send(message, 400, "Bad Request");
    return;
  };

  // Create a new Call
  auto call = std::make_shared<Call>(message->header->headers_map["Call-ID"][0]->as<StringHeader>()->to_string());
  call->from = message->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;
  call->to = message->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value;
  call->add_session(message->session);

  // Create RTP/RTCP Relay Pair
  call->rtp_set = registrar->rtprelay_allocate();
  call->rtcp_set = registrar->rtprelay_allocate();
  call->rtp_set->start();
  call->rtcp_set->start();

  // Rewrite SDP
  _rewrite_sdp(sdp, registrar->config->rtprelay_public_address, call->rtp_set->port, call->rtcp_set->port);
  logger->debug("[Call " + call->id + "] Modifed SFP for RTPRelay (INVITE)");
  message->body = sdp->to_string();
  // sdp->print();

  // Is To: a subscriber?
  auto toSubscriber = registrar->subscriber_get(call->to);

  // Not Found TODO:Forwarding?
  if (!toSubscriber) {
    logger->debug("INVITE - To: " + call->to->to_string() + " Not Found, Sending 404 Not Found");
    _send(message, 404, "Not Found");
    return;
  }
  logger->debug("INVITE - Found To: " + call->to->to_string() + " Subscriber: " + std::to_string(toSubscriber->id));

  // Is Subscriber Online? TODO: This should query other AthenaSIP Instances if not.
  auto other_session = registrar->subscriber_get_session(toSubscriber);

  // Not Found TODO:Forwarding?
  if (!other_session) {
    call->add_session(other_session);
    logger->debug("INVITE - To: " + call->to->to_string() + " Session Not Found, Sending 404 Not Found");
    _send(message, 404, "Not Found");
    return;
  }

  logger->info("Registering Call " + call->id);
  registrar->call_register(call->id, call);

  // Forward INVITE to other party
  other_session->send(message);
}

void SIPCore::_rewrite_sdp(std::shared_ptr<SDP> sdp, std::string server_address, uint16_t rtp_port, uint16_t rtcp_port) {
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

void SIPCore::_send(std::shared_ptr<SIPMessage> message, uint16_t code, std::string response_message) {
  message->header->response_code = code;
  message->header->response_message = response_message;
  message->session->send(message);
}

}  // namespace athenasip
