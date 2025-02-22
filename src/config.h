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

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "types/url.h"

using namespace athenasip::loggers;

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);

  // SIP configuration
  std::string sip_realm;                     // e.g. "example.com"
  std::string sip_nonce_secret;              // e.g. "your_nonce_secret_here"
  uint32_t sip_registration_timeout = 5000;  // (set a default or load from YAML)

  // TLS Configuration
  bool tls_enable;
  std::string tls_address;            // e.g. "127.0.0.1"
  uint16_t tls_port = 0;              // e.g. 5061
  std::string tls_cert_pem_filename;  // e.g. "./tls/snakeoil.cer"
  std::string tls_key_pem_filename;   // e.g. "./tls/snakeoil.key"

  // TCP Configuration
  bool tcp_enable;
  std::string tcp_address;  // e.g. "127.0.0.1"
  uint16_t tcp_port = 0;    // e.g. 5060

  // DB configuration
  std::string db_database_url;  // e.g. "mysql://root@localhost/"

  // SQLLite configuration (optional)
  bool sqlite_create_database;  // sqllite.create_database - True if the database should be created

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string &filename);

 private:
  std::shared_ptr<Logger> _logger;
};

}  // namespace athenasip
