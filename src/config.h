//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "logger.h"
#include "logger_scoped.h"
#include "types/url.h"

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);

  // SIP configuration
  std::string address;             // e.g. "127.0.0.1"
  uint16_t port;                   // e.g. 5060
  std::string realm;               // e.g. "example.com"
  std::string nonce_secret;        // e.g. "your_nonce_secret_here"
  std::string cert_pem_filename;   // e.g. "./tls/snakeoil.cer"
  std::string key_pem_filename;    // e.g. "./tls/snakeoil.key"

  // DB configuration
  std::string database_url;        // e.g. "mysql://root@localhost/"

  // Other configuration options
  uint32_t registration_timeout;   // (set a default or load from YAML)
  uint32_t babble_limit;           // (set a default or load from YAML)

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string &filename);

  // Retrieves a value using a simple path lookup (e.g. "sip.address").
  std::optional<std::string> get(const std::string &path);

 private:
  std::shared_ptr<Logger> _logger;
};

}  // namespace athenasip
