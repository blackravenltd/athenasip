//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mysqlx/xdevapi.h>

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "logger.h"
#include "logger_scoped.h"

namespace athenasip {

class Registrar {
 public:
  Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<mysqlx::Session> db);
  ~Registrar();

  bool user_exists(const std::string& identity);
  std::string user_get_h1(const std::string& identity);
  void user_register(const std::string& identity, const std::string& location);
  void user_get_location(const std::string& identity);

 private:
  std::unique_ptr<Logger> _logger;
  std::shared_ptr<mysqlx::Session> _session;
};

}  // namespace athenasip