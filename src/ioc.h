//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "config.h"
#include "db.h"
#include "logger.h"
#include "registrar.h"
#include "tls_server.h"
#include "version.h"

namespace athenasip {

class IOC {
 public:
  std::shared_ptr<Version> version;
  std::shared_ptr<Logger> logger;
  std::shared_ptr<Config> config;
  std::shared_ptr<DB> db;
  std::shared_ptr<TLSServer> server;
  std::shared_ptr<Registrar> registrar;
};

}  // namespace athenasip