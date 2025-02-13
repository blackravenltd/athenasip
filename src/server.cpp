//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "server.h"

namespace athenasip {

Server::Server(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string _nonce_secret)
    : _logger(logger), _registrar(registrar), nonce_secret(_nonce_secret) {}

void Server::register_connection(std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_connections_mtx);
  _connections.insert({session->remote_endpoint, session});
  _logger->debug("Registered Connection " + session->remote_endpoint);
}

void Server::unregister_connection(std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_connections_mtx);
  _connections.erase(session->remote_endpoint);
  _logger->debug("Unregistered Connection " + session->remote_endpoint);
}

}  // namespace athenasip