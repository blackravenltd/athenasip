//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "server.h"

namespace athenasip {

Server::Server(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string _nonce_secret)
    : _logger(logger), _registrar(registrar), nonce_secret(_nonce_secret) {}

bool Server::register_session(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  _sessions.insert({endpoint, session});
  _logger->debug("Registered Connection " + endpoint);
  return true;
}

bool Server::unregister_session(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  _sessions.erase(endpoint);
  _logger->debug("Unregistered Connection " + endpoint);
  return true;
}

std::optional<std::string> Server::subscriber_get_h1(std::shared_ptr<SIPIdentity> identity, std::shared_ptr<Session> session) {
  return _registrar->subscriber_get_h1(identity);
}

}  // namespace athenasip