//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <string>

#include "logger.h"
#include "logger_scoped.h"
#include "url.h"

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);
  
  std::string database_url;
  uint32_t registration_timeout;
  uint32_t babble_limit;

  bool load_from_yaml(std::string &filename);

 private:
  std::unique_ptr<Logger> _logger;
};

}  // namespace athenasip
