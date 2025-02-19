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

bool Server::session_register(std::string endpoint, std::shared_ptr<Session> session) {
  return _registrar->session_register(endpoint, session);
}

bool Server::session_unregister(std::string endpoint, std::shared_ptr<Session> session) {
  return _registrar->session_unregister(endpoint, session);
}

}  // namespace athenasip