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
#include <vector>

#include "config.h"
#include "databases/db.h"
#include "loggers/logger.h"
#include "registrar.h"
#include "servers/server.h"
#include "rtp/rtp_relay.h"
#include "version.h"

namespace athenasip {

class SIPCore {
 public:
  SIPCore();
  SIPCore(std::shared_ptr<Version> _version, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::loggers::Logger> _logger,
          std::shared_ptr<athenasip::databases::DB> _db, std::shared_ptr<Registrar> _registrar);

 public:
  std::shared_ptr<Version> version;
  std::shared_ptr<Config> config;
  std::shared_ptr<athenasip::loggers::Logger> logger;
  std::shared_ptr<athenasip::databases::DB> db;
  std::shared_ptr<rtp::RTPRelay> rtprelay;
  std::shared_ptr<Registrar> registrar;
  std::vector<std::shared_ptr<Server>> servers;
};

}  // namespace athenasip
