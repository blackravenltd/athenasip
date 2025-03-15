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
  std::string sip_realm;
  std::string sip_nonce_secret;
  uint32_t sip_nonce_expiry = 3600;
  uint32_t sip_registration_timeout = 5000;

  // TLS Configuration
  bool tls_enable;
  std::string tls_address;
  uint16_t tls_port = 0;
  std::string tls_cert_pem_filename;
  std::string tls_key_pem_filename;

  // TCP Configuration
  bool tcp_enable;
  std::string tcp_address;  // e.g. "127.0.0.1"
  uint16_t tcp_port = 0;    // e.g. 5060

  // UDP Configuration
  bool udp_enable;
  std::string udp_address;  // e.g. "127.0.0.1"
  uint16_t udp_port = 0;    // e.g. 5060

  // DB configuration
  std::string db_database_url;  // e.g. "mysql://root@localhost/"

  // SQLLite configuration (optional)
  bool sqlite_create_database;  // sqllite.create_database - True if the database should be created

  // RTPProxyClient configuration
  bool rtprelay_enable;
  std::string rtprelay_address;
  std::string rtprelay_public_address;
  uint16_t rtprelay_min_port;
  uint16_t rtprelay_max_port;

  bool admin_api_enable;
  std::string admin_api_address;
  uint16_t admin_api_port = 0;  // e.g. 5060

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string &filename);

 private:
  std::shared_ptr<Logger> _logger;
};

}  // namespace athenasip
