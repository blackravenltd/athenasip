//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "config.h"
#include "db.h"
#include "logger.h"
#include "registrar.h"
#include "server.h"
#include "version.h"

namespace athenasip {

class SIPCore {
 public:
  SIPCore();
  SIPCore(std::shared_ptr<Version> _version, std::shared_ptr<Config> _config, std::shared_ptr<Logger> _logger, std::shared_ptr<DB> _db,
          std::shared_ptr<Registrar> _registrar, std::shared_ptr<Server> _server);

 public:
  std::shared_ptr<Version> version;
  std::shared_ptr<Config> config;
  std::shared_ptr<Logger> logger;
  std::shared_ptr<DB> db;
  std::shared_ptr<Registrar> registrar;
  std::shared_ptr<Server> server;
};

}  // namespace athenasip
