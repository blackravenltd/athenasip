//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "db.h"
#include "logger.h"
#include "logger_scoped.h"
#include "sip_identity.h"

namespace athenasip {

class Registrar {
 public:
  Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<DB> db);

  bool user_exists(const SIPIdentity& identity);
  std::string user_get_h1(const SIPIdentity& identity);
  void user_register(const SIPIdentity& identity, const SIPUri& location);
  const std::shared_ptr<SIPUri> user_get_location(const SIPIdentity& identity);

 private:
  std::unique_ptr<Logger> _logger;
  std::shared_ptr<DB> _db;
};

}  // namespace athenasip