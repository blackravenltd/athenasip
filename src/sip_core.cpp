//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_core.h"

#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "util.h"

namespace athenasip {

SIPCore::SIPCore(std::shared_ptr<Logger> logger, std::shared_ptr<Version> version, std::shared_ptr<Config> config, std::shared_ptr<Registrar> _registrar)
    : _logger(logger), _version(version), _config(config), registrar(_registrar) {}

void SIPCore::process_message(std::shared_ptr<SIPMessage> message) {
  // Message must contain Via and CSeq to identify the transaction
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("[Request] - Incomplete Headers (No Via/CSeq) - Sending 400 Bad Request");

    // Send 400 Bad Request
    auto response = message->generate_response();
    response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
    response->header->response_code = 400;
    response->header->response_message = "Bad Request";
    response->channel->send(response);

    return;
  }

  // Find or create the message transaction
  auto transactionId = message->get_transaction_id();
  _logger->debug("[Request] - Transaction is " + transactionId);
  message->transaction = registrar->transaction_get(transactionId);
  if (!message->transaction) {
    message->transaction = std::make_shared<Transaction>(_logger, message->channel, registrar, Transaction::Direction::Incoming, transactionId);
    message->transaction->type = (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE")
                                     ? Transaction::Type::INVITE
                                     : Transaction::Type::NonINVITE;
    message->transaction->start(_config->sip_timer_t1_rtt_ms);
  } else {
    message->transaction->reset_timers();
  }

  // Parse the Message in the context of the transaction
  message->transaction->receive_message(message);

  // REGISTER?
  if (message->header->request_method == "REGISTER") {
    if (!message->header->contains("Authorization")) {
      // No Authorization Header
      _logger->info("[" + message->header->request_method + "] - No Authorization Header");
      return send_401_unauthorized(message);
    }

    // Process Authorization

    auto auth = message->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
    if (auth->type != "Digest" || !auth->contains_field("realm") || !auth->contains_field("nonce") || !auth->contains_field("response") ||
        !auth->contains_field("uri")) {
      // Auth fields incorrect
      _logger->info("[" + message->header->request_method + "] - Incomplete Authorization Fields");
      return send_401_unauthorized(message);
    }

    if (!registrar->nonce_check(auth->fields["nonce"])) {
      // No Nonce exists or is expired
      _logger->info("[" + message->header->request_method + "] - Nonce " + auth->fields["nonce"] + " Not Found or Expired");
      return send_401_unauthorized(message);
    }

    auto aorSubscriber = registrar->subscriber_get(message->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value);

    if (!aorSubscriber) {
      // Subscriber not found
      _logger->info("[" + message->header->request_method + "] - Subscriber " + aorSubscriber->identity->to_string() + " Not Found");
      return send_401_unauthorized(message);
    }

    // Generate MD5 Check
    auto authMD5 = Util::md5(aorSubscriber->ha1 + ":" + auth->fields["nonce"] + ":" + Util::md5(message->header->request_method + ":" + auth->fields["uri"]));

    if (Util::to_lower(authMD5) != Util::to_lower(auth->fields["response"])) {
      // Authorization rejected
      _logger->info("[" + message->header->request_method + "] - MD5 Authorization Failed");
      return send_401_unauthorized(message);
    }

    // Send 200 OK
    auto response = message->generate_response();
    response->header->response_code = 200;
    response->header->response_message = "OK";
    response->channel->send(response);
  }
}

void SIPCore::send_401_unauthorized(std::shared_ptr<SIPMessage> message) {
  // Send 401 Unauthorized
  auto response = message->generate_response();
  response->header->add("Reason", "SIP;cause=401;text=\"Unauthorized\"");
  response->header->response_code = 401;
  response->header->response_message = "Unauthorized";
  // Get From Identity
  auto toIdentity = message->header->headers_map["From"][0]->as<headers::SIPIdentityHeader>()->value;
  // Don't send WWW-Authenticate if realm not recognised
  if (registrar->realm_exists(toIdentity->uri->realm)) {
    // Generate Nonce
    response->header->add("WWW-Authenticate", "Digest realm=\"" + toIdentity->uri->realm + "\", nonce=\"" + registrar->nonce_get() + "\"");
  }
  // Send Response
  response->channel->send(response);
}

}  // namespace athenasip
