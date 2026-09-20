//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

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
  std::string sip_node_id;
  uint32_t sip_registration_timeout = 5000;

  // Allow SIP over an unencrypted transport. WSS is required for browsers, and TLS is
  // what a cluster talks, so this is the switch that permits plain UDP, TCP and WS.
  bool sip_allow_unencrypted = true;

  std::string sip_event_prefix;

  // RFC 4028 section 5: the shortest session interval this node will let a call
  // negotiate, and the floor the RFC itself sets is 90 seconds. A proxy that keeps state
  // for a call has a stake in how often it is told the call is still there, and rejecting
  // an interval below this is the only say it gets (section 8).
  uint32_t sip_session_min_se = 90;

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
  uint16_t sip_timer_j_server_non_invite_duration = 64;
  uint16_t sip_timer_k_non_invite_duration = 1;

  bool sip_timer_reliable_transport_retransmits = false;

  // TLS Configuration
  bool tls_enable = false;
  std::string tls_address;
  uint16_t tls_port = 0;
  std::string tls_cert_pem_filename;
  std::string tls_key_pem_filename;

  // TCP Configuration
  bool tcp_enable = false;
  std::string tcp_address;  // e.g. "127.0.0.1"
  uint16_t tcp_port = 0;    // e.g. 5060

  // UDP Configuration
  bool udp_enable = false;
  std::string udp_address;  // e.g. "127.0.0.1"
  uint16_t udp_port = 0;    // e.g. 5060

  // UDP Configuration
  bool websocket_enable = false;
  std::string websocket_address;  // e.g. "127.0.0.1"
  uint16_t websocket_port = 0;    // e.g. 5060

  // RFC 7118 over TLS. A browser will not open an insecure WebSocket from a page served
  // over https, so this is what a web client actually connects to; ws:// is for local
  // development. The certificate is the listener's own rather than the tls section's,
  // because the name a browser reaches the node by is rarely the name a SIP peer does.
  bool websocket_tls = false;
  std::string websocket_cert_pem_filename;
  std::string websocket_key_pem_filename;

  // DB configuration
  std::string db_url;  // e.g. "memory://" or "redis://127.0.0.1:6379"

  // Events configuration
  std::string events_url;  // e.g. "mqtt://user:pass@127.0.0.1:1883/athenasip?client_id=sip-01&keep_alive=30"

  // Media configuration
  std::string media_url = "builtin://";  // or "rtpengine://host:port"

  // RTPProxyClient configuration
  bool rtprelay_enable = false;
  std::string rtprelay_address;
  std::string rtprelay_public_address;
  uint16_t rtprelay_min_port = 22000;
  uint16_t rtprelay_max_port = 23000;

  // HTTP configuration
  std::string http_address;
  uint16_t http_port = 0;  // e.g. 5060
  bool http_api_enable = false;
  bool http_files_enable = false;
  std::string http_files_path;

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string& filename);

  // A plugin's own configuration: the section named after the driver, inside the
  // section for its kind. The URL stays the selector, so this is only for what a URL
  // cannot express (engine pools, health-check intervals) and is usually absent.
  //
  //   datastore:
  //     url: redis://127.0.0.1:6379
  //     redis:
  //       health_check_interval: 5
  //
  // Keyed on the driver's name() rather than the scheme it was reached by, so the
  // three Redis schemes share one section instead of needing three.
  YAML::Node plugin_root(const std::string& kind, const std::string& name) const;

 private:
  std::shared_ptr<Logger> _logger;

  // The document as loaded, kept so plugins can be handed their own section. Nothing
  // else should read it: everything the server itself needs is parsed into the fields
  // above at load time.
  YAML::Node _root;
};

}  // namespace athenasip
