//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "db.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "types/sip_identity.h"

using namespace athenasip::types;
using namespace athenasip::loggers;

namespace athenasip {

class Registrar {
 public:
  Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<DB> db);

  bool subscriber_exists(std::shared_ptr<SIPIdentity> identity);
  std::optional<std::string> subscriber_get_h1(std::shared_ptr<SIPIdentity> identity);
  void subscriber_register(std::shared_ptr<SIPIdentity> identity, std::shared_ptr<SIPUri> location);
  const std::shared_ptr<SIPUri> subscriber_get_location(std::shared_ptr<SIPIdentity> identity);

 private:
  std::unique_ptr<Logger> _logger;
  std::shared_ptr<DB> _db;
};

}  // namespace athenasip