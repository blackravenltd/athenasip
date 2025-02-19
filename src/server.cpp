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
  return _registrar->register_session(endpoint, session);
}

bool Server::unregister_session(std::string endpoint, std::shared_ptr<Session> session) {
  return _registrar->unregister_session(endpoint, session);
}

}  // namespace athenasip