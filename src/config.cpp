//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

namespace athenasip {

Config::Config(std::shared_ptr<Logger> logger) : _logger(std::make_unique<LoggerScoped>("config", logger)) {}

bool Config::load_from_yaml(std::string &filename) { return true; }

std::optional<std::string> Config::get(std::string &path) {
  if (path == "db:url") {
    return "mysqlx://root@localhost/athenasip";
  }

  return nullptr;
}

}  // namespace athenasip