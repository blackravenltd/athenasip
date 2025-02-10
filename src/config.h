//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/program_options.hpp>
#include <cstdint>
#include <string>

#include "logger.h"
#include "logger_scoped.h"
#include "url.h"

namespace athenasip {

enum DBMode { FILE, MYSQL, POSTGRESQL };

class Config {
 public:
  DBMode dbMode;
  URL dbUrl;
  uint16_t port;

  Config(std::shared_ptr<Logger> logger);
  Config(std::shared_ptr<Logger> logger, int argc, char* argv[]);

  void parse_cmd_line(int argc, char* argv[]);
  bool is_valid();

 private:
  std::unique_ptr<Logger> _logger;

  bool _valid;

  static const std::map<std::string, uint16_t> defaultPorts;
};

}  // namespace athenasip
