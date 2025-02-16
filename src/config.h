//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <string>

#include "logger.h"
#include "logger_scoped.h"
#include "types/url.h"

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);

  uint16_t port;
  std::string database_url;
  uint32_t registration_timeout;
  uint32_t babble_limit;
  std::string nonce_secret;

  bool load_from_yaml(std::string &filename);

  std::optional<std::string> get(std::string &path);

 private:
  std::unique_ptr<Logger> _logger;
};

}  // namespace athenasip
