//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "server.h"

namespace athenasip::servers {

Server::Server(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string _nonce_secret)
    : _logger(logger), _registrar(registrar), nonce_secret(_nonce_secret) {}

void Server::set_session_events(std::shared_ptr<Session> new_session) {
  new_session->on_start([this](std::string endpoint, std::shared_ptr<Session> session) { return _registrar->session_register(endpoint, session); });
  new_session->on_close([this](std::string endpoint, std::shared_ptr<Session> session) { return _registrar->session_unregister(endpoint, session); });
  new_session->on_authenticate(
      [this](std::shared_ptr<SIPIdentity> identity, std::shared_ptr<Session> session) { return _registrar->subscriber_get(identity); });
  new_session->on_register_location([this](std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session) {
    return _registrar->subscriber_register(subscriber, contact, session);
  });
  new_session->on_unregister_location([this](std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session) {
    return _registrar->subscriber_unregister(subscriber, contact, session);
  });
  new_session->on_get_subscriber_session([this](std::shared_ptr<Subscriber> subscriber) { return _registrar->subscriber_get_session(subscriber); });
}

}  // namespace athenasip::servers