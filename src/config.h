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
  uint32_t sip_nonce_expiry = 3600;
  uint32_t sip_registration_timeout = 5000;

  // SIP Timers
  uint16_t sip_timer_t1_rtt_ms = 500;
  uint16_t sip_timer_t2_max_retransmit_interval_ms = 4000;
  uint16_t sip_timer_t4_network_propagation_ms = 5000;
  uint16_t sip_timer_a_invite_initial = 1;
  uint16_t sip_timer_b_invite_timeout = 64;
  uint16_t sip_timer_d_invite_duration = 64;
  uint16_t sip_timer_e_non_invite_initial = 1;
  uint16_t sip_timer_f_non_invite_timeout = 64;
  uint16_t sip_timer_g_server_invite_initial = 1;
  uint16_t sip_timer_h_server_invite_timeout = 64;
  uint16_t sip_timer_i_server_invite_duration = 1;

  bool sip_timer_reliable_transport_retransmits = false;

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

  // UDP Configuration
  bool websocket_enable;
  std::string websocket_address;  // e.g. "127.0.0.1"
  uint16_t websocket_port = 0;    // e.g. 5060

  // DB configuration
  std::string db_url;  // e.g. "mysql://root@localhost/"
  bool db_create = false;

  // MQTT configuration
  std::string mqtt_broker_host;  // e.g. "127.0.0.1"
  std::uint16_t mqtt_broker_port = 1883;
  std::string mqtt_client_id = "athenasip-events";
  std::string mqtt_username;
  std::string mqtt_password;
  std::uint16_t mqtt_keep_alive_seconds = 3;

  // SQLLite configuration (optional)
  bool sqlite_create_database;  // sqllite.create_database - True if the database should be created

  // RTPProxyClient configuration
  bool rtprelay_enable;
  std::string rtprelay_address;
  std::string rtprelay_public_address;
  uint16_t rtprelay_min_port;
  uint16_t rtprelay_max_port;

  // HTTP configuration
  std::string http_address;
  uint16_t http_port = 0;  // e.g. 5060
  bool http_api_enable;
  bool http_files_enable;
  std::string http_files_path;

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string& filename);

 private:
  std::shared_ptr<Logger> _logger;
};

}  // namespace athenasip
