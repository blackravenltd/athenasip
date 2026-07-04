//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "transaction.h"

#include "registrar.h"
#include "sip_message.h"

using namespace athenasip::loggers;

namespace athenasip {

Transaction::Transaction(std::shared_ptr<Logger> logger, std::shared_ptr<Channel> channel, std::shared_ptr<Registrar> registrar, Direction direction,
                         std::string _id)
    : _logger(std::make_unique<LoggerScoped>("transaction " + _id, logger)), _channel(channel), _registrar(registrar), _direction(direction), id(_id) {}

Transaction::~Transaction() {}

void Transaction::start(uint16_t t1_ms) {
  _t1_ms = t1_ms;
  _logger->debug("start");
  _replace_timer();
}

void Transaction::reset_timers() {
  _logger->debug("reset timers");
  _replace_timer();
}

void Transaction::end() {
  _logger->debug("end");
  _ended = true;
  if (_timer_b_f != nullptr) _timer_b_f->cancel();
  if (_registrar) _registrar->transaction_unregister(id);
}

void Transaction::receive_message(std::shared_ptr<SIPMessage> message) {
  if (_ended) {
    _logger->debug("receive_message " + message->header->first_line() + " (ended, dropping)");
    return;
  }

  _logger->debug("receive_message " + message->header->first_line());

  if (message->header->request_method == "REGISTER") {
    process_register(message);
  } else if (message->header->request_method == "ACK") {
    process_ack(message);
  } else {
    process_unknown(message);
  }
}

void Transaction::process_register(std::shared_ptr<SIPMessage> message) {
  // No Authorization
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

  if (!_registrar->nonce_check(auth->fields["nonce"])) {
    // No Nonce exists or is expired
    _logger->info("[" + message->header->request_method + "] - Nonce " + auth->fields["nonce"] + " Not Found or Expired");
    return send_401_unauthorized(message);
  }

  auto& identityRef = message->header->headers_map["To"][0]->as<SIPIdentityHeader>()->value;
  auto aorSubscriber = _registrar->subscriber_get(identityRef);

  if (aorSubscriber == nullptr) {
    // Subscriber not found
    _logger->info("[" + message->header->request_method + "] - Subscriber " + identityRef->to_string() + " Not Found");
    return send_401_unauthorized(message);
  }

  // Generate MD5 Check
  auto authMD5 = Util::to_lower(
      Util::md5(aorSubscriber->ha1 + ":" + auth->fields["nonce"] + ":" + Util::md5(message->header->request_method + ":" + auth->fields["uri"])));

  if (authMD5 != Util::to_lower(auth->fields["response"])) {
    // Authorization rejected
    _logger->info("[" + message->header->request_method + "] - MD5 Authorization Failed");
    return send_401_unauthorized(message);
  }

  // Send 200 OK
  auto response = message->generate_response();
  response->header->response_code = 200;
  response->header->response_message = "OK";
  if (message->header->contains("Contact")) {
    auto contact_header = message->header->headers_map["Contact"][0]->as<SIPIdentityHeader>();

    if (contact_header && contact_header->value) {
      auto contact = std::make_shared<SIPIdentity>(*contact_header->value);
      // TODO: This should be configurable
      contact->tags["expires"] = "3600";

      response->header->add("Contact", std::make_shared<SIPIdentityHeader>(contact));
    }
  }
  response->channel->send(response);

  // End this transaction
  end();
}

void Transaction::process_ack(std::shared_ptr<SIPMessage> message) {
  _logger->debug("Received ACK, no response");

  // End this transaction
  end();
}

void Transaction::process_unknown(std::shared_ptr<SIPMessage> message) {
  _logger->warn("Unknown request method " + message->header->request_method);

  // Send 501 Not Implemented
  auto response = message->generate_response();
  response->header->add("Reason", "SIP;cause=501;text=\"Not Implemented\"");
  response->header->response_code = 501;
  response->header->response_message = "Not Implemented";
  response->channel->send(response);

  // End this transaction
  end();
}

void Transaction::send_message(std::shared_ptr<SIPMessage> message) { _logger->debug("send_message " + message->header->first_line()); }

void Transaction::send_401_unauthorized(std::shared_ptr<SIPMessage> message) {
  // Send 401 Unauthorized
  auto response = message->generate_response();
  response->header->add("Reason", "SIP;cause=401;text=\"Unauthorized\"");
  response->header->response_code = 401;
  response->header->response_message = "Unauthorized";
  // Get From Identity
  auto toIdentity = message->header->headers_map["From"][0]->as<headers::SIPIdentityHeader>()->value;
  // Send WWW-Authenticate if realm recognised
  auto realm = _registrar->realm_get_by_name(Util::to_lower(toIdentity->uri->realm));
  if (realm) {
    // Generate Nonce
    response->header->add("WWW-Authenticate", "Digest realm=\"" + realm->name + "\", nonce=\"" + _registrar->nonce_create(realm) + "\"");
  }
  // Send Response
  response->channel->send(response);

  // End this transaction
  end();
}

void Transaction::_replace_timer() {
  auto self = shared_from_this();

  // Cancel and explicitly free old B/F timer
  if (_timer_b_f != nullptr) _timer_b_f->cancel();
  _timer_b_f.reset();

  // Create new B/F timer
  _timer_b_f = DelayedTask<int>::schedule(
      [this, self]() -> int {
        _timer_b_f.reset();
        _timeout();
        return 0;
      },
      _t1_ms * 10);

  _logger->debug("B/F timer set");
}

void Transaction::_timeout() {
  _logger->debug("B/F timeout");
  end();
}

}  // namespace athenasip