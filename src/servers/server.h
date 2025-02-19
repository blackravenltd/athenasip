//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../registrar.h"
#include "../session.h"

using namespace athenasip::loggers;

namespace athenasip::servers {

class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string _nonce_secret);

  virtual void start() = 0;
  virtual void stop() = 0;

  virtual bool session_register(std::string endpoint, std::shared_ptr<Session> session);
  virtual bool session_unregister(std::string endpoint, std::shared_ptr<Session> session);

  std::string nonce_secret = "testing123";

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Registrar> _registrar;
};

}  // namespace athenasip::servers
