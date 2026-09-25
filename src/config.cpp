//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace athenasip {

Config::Config(std::shared_ptr<Logger> logger) { _logger = std::make_unique<LoggerScoped>("config", logger); }

bool Config::load_from_yaml(const std::string& filename) {
  YAML::Node config;
  try {
    config = YAML::LoadFile(filename);
  } catch (const YAML::Exception& e) {
    _logger->error("Failed to parse YAML file: " + std::string(e.what()));
    return false;
  }

  _root = config;

  // --- Parse the 'sip' section ---
  if (!config["sip"]) {
    _logger->error("YAML file missing 'sip' section");
    return false;
  }
  YAML::Node sip = config["sip"];

  if (sip["node_id"]) {
    sip_node_id = sip["node_id"].as<std::string>();
  } else {
    _logger->error("Missing 'sip.node_id'");
    return false;
  }

  // Previously present in the YAML and the docs but never read, so setting it had no
  // effect at all.
  if (sip["allow_unencrypted"]) sip_allow_unencrypted = sip["allow_unencrypted"].as<bool>();
  if (sip["log_messages"]) sip_log_messages = sip["log_messages"].as<bool>();
  if (sip["public_address"]) sip_public_address = sip["public_address"].as<std::string>();
  if (sip["media_timeout"]) sip_media_timeout = sip["media_timeout"].as<uint32_t>();
  if (sip["max_call_duration"]) sip_max_call_duration = sip["max_call_duration"].as<uint32_t>();
  if (sip["require_session_timer"]) sip_require_session_timer = sip["require_session_timer"].as<bool>();

  if (sip["session_expires"]) {
    const auto configured = sip["session_expires"].as<uint32_t>();

    // RFC 4028 section 4: "SIP entities MUST be prepared to handle Session-Expires header
    // field values of any duration greater than 90 seconds, but entities that insert the
    // Session-Expires header field SHOULD NOT choose values of less than 30 minutes."
    // Zero is not a short interval, it is the switch that says not to insert one.
    if (configured != 0 && configured < 90) {
      _logger->error("sip.session_expires must be at least 90 (RFC 4028) - keeping " + std::to_string(sip_session_expires));
    } else {
      if (configured != 0 && configured < 1800) _logger->warn("sip.session_expires below the 1800 RFC 4028 recommends for an inserted interval");
      sip_session_expires = configured;
    }
  }

  if (sip["session_min_se"]) {
    const auto configured = sip["session_min_se"].as<uint32_t>();

    // RFC 4028 section 8.1: the minimum a proxy quotes in a 422 "MUST NOT be lower than
    // 90 seconds", which section 4 explains is a bit more than twice the longest a SIP
    // transaction can take. Below it a refresh could not complete before the session it
    // was refreshing expired.
    if (configured < 90) {
      _logger->error("sip.session_min_se must be at least 90 (RFC 4028) - keeping " + std::to_string(sip_session_min_se));
    } else {
      sip_session_min_se = configured;
    }
  }

  // SIP Timers
  YAML::Node sip_timers = sip["timers"];
  if (sip_timers) {
    if (sip_timers["t1_rtt_ms"]) sip_timer_t1_rtt_ms = sip_timers["t1_rtt_ms"].as<uint16_t>();
    if (sip_timers["t2_max_retransmit_interval_ms"]) sip_timer_t2_max_retransmit_interval_ms = sip_timers["t2_max_retransmit_interval_ms"].as<uint16_t>();
    if (sip_timers["t4_network_propagation_ms"]) sip_timer_t4_network_propagation_ms = sip_timers["t4_network_propagation_ms"].as<uint16_t>();
    if (sip_timers["a_invite_initial"]) sip_timer_a_invite_initial = sip_timers["a_invite_initial"].as<uint16_t>();
    if (sip_timers["b_invite_timeout"]) sip_timer_b_invite_timeout = sip_timers["b_invite_timeout"].as<uint16_t>();
    if (sip_timers["d_invite_duration"]) sip_timer_d_invite_duration = sip_timers["d_invite_duration"].as<uint16_t>();
    if (sip_timers["e_non_invite_initial"]) sip_timer_e_non_invite_initial = sip_timers["e_non_invite_initial"].as<uint16_t>();
    if (sip_timers["f_non_invite_timeout"]) sip_timer_f_non_invite_timeout = sip_timers["f_non_invite_timeout"].as<uint16_t>();
    if (sip_timers["g_server_invite_initial"]) sip_timer_g_server_invite_initial = sip_timers["g_server_invite_initial"].as<uint16_t>();
    if (sip_timers["h_server_invite_timeout"]) sip_timer_h_server_invite_timeout = sip_timers["h_server_invite_timeout"].as<uint16_t>();
    if (sip_timers["i_server_invite_duration"]) sip_timer_i_server_invite_duration = sip_timers["i_server_invite_duration"].as<uint16_t>();
    if (sip_timers["j_server_non_invite_duration"]) sip_timer_j_server_non_invite_duration = sip_timers["j_server_non_invite_duration"].as<uint16_t>();
    if (sip_timers["k_non_invite_duration"]) sip_timer_k_non_invite_duration = sip_timers["k_non_invite_duration"].as<uint16_t>();

    if (sip_timers["c_invite_proxy_ms"]) {
      const auto configured = sip_timers["c_invite_proxy_ms"].as<uint32_t>();

      // RFC 3261 16.6 step 11: "The timer MUST be larger than 3 minutes." A shorter one
      // would give up on calls that are only still ringing, so it is refused rather
      // than honoured.
      if (configured <= 180000) {
        _logger->error("sip.timers.c_invite_proxy_ms must be larger than 180000 - keeping " + std::to_string(sip_timer_c_invite_proxy_ms));
      } else {
        sip_timer_c_invite_proxy_ms = configured;
      }
    }
  }

  if (sip["connect_timeout_ms"]) sip_connect_timeout_ms = sip["connect_timeout_ms"].as<uint32_t>();

  // --- Parse the 'tls' section ---
  YAML::Node tls = config["tls"];
  if (tls) {
    if (tls["enable"])
      tls_enable = tls["enable"].as<bool>();
    else
      tls_enable = true;

    if (tls["address"])
      tls_address = tls["address"].as<std::string>();
    else
      tls_address = "0.0.0.0";

    if (tls["port"]) {
      try {
        tls_port = tls["port"].as<uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'tls.port': " + std::string(e.what()));
        return false;
      }
    } else if (tls_enable) {
      // Only when the listener is switched on. A section that says enable: false and
      // nothing else is a listener turned off, not a configuration error, and refusing
      // to start over the port of something that will never listen is the kind of thing
      // that makes a server feel hostile to configure.
      _logger->error("Missing 'tls.port'");
      return false;
    } else {
      tls_port = 5061;
    }

    if (tls["cert_pem_filename"])
      tls_cert_pem_filename = tls["cert_pem_filename"].as<std::string>();
    else {
      _logger->warn("Missing 'tls.cert_pem_filename'");
      tls_cert_pem_filename.clear();
    }

    if (tls["key_pem_filename"])
      tls_key_pem_filename = tls["key_pem_filename"].as<std::string>();
    else {
      _logger->warn("Missing 'tls.key_pem_filename'");
      tls_key_pem_filename.clear();
    }
  }

  // --- Parse the 'tls' section ---
  YAML::Node tcp = config["tcp"];
  if (tcp) {
    if (tcp["enable"])
      tcp_enable = tcp["enable"].as<bool>();
    else
      tcp_enable = true;

    if (tcp["address"])
      tcp_address = tcp["address"].as<std::string>();
    else
      tcp_address = "0.0.0.0";

    if (tcp["port"]) {
      try {
        tcp_port = tcp["port"].as<uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'tcp.port': " + std::string(e.what()));
        return false;
      }
    } else if (tcp_enable) {
      // Only when the listener is switched on. A section that says enable: false and
      // nothing else is a listener turned off, not a configuration error, and refusing
      // to start over the port of something that will never listen is the kind of thing
      // that makes a server feel hostile to configure.
      _logger->error("Missing 'tcp.port'");
      return false;
    } else {
      tcp_port = 5060;
    }
  }

  // --- Parse the 'udp' section ---
  YAML::Node udp = config["udp"];
  if (udp) {
    if (udp["enable"])
      udp_enable = udp["enable"].as<bool>();
    else
      udp_enable = true;

    if (udp["address"])
      udp_address = udp["address"].as<std::string>();
    else
      udp_address = "0.0.0.0";

    if (udp["port"]) {
      try {
        udp_port = udp["port"].as<uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'udp.port': " + std::string(e.what()));
        return false;
      }
    } else if (udp_enable) {
      // Only when the listener is switched on. A section that says enable: false and
      // nothing else is a listener turned off, not a configuration error, and refusing
      // to start over the port of something that will never listen is the kind of thing
      // that makes a server feel hostile to configure.
      _logger->error("Missing 'udp.port'");
      return false;
    } else {
      udp_port = 5060;
    }
  }

  // --- Parse the 'websocket' section ---
  YAML::Node websocket = config["websocket"];
  if (websocket) {
    if (websocket["enable"])
      websocket_enable = websocket["enable"].as<bool>();
    else
      websocket_enable = true;

    if (websocket["address"])
      websocket_address = websocket["address"].as<std::string>();
    else
      websocket_address = "0.0.0.0";

    if (websocket["port"]) {
      try {
        websocket_port = websocket["port"].as<uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'websocket.port': " + std::string(e.what()));
        return false;
      }
    } else if (websocket_enable) {
      // Only when the listener is switched on. A section that says enable: false and
      // nothing else is a listener turned off, not a configuration error, and refusing
      // to start over the port of something that will never listen is the kind of thing
      // that makes a server feel hostile to configure.
      _logger->error("Missing 'websocket.port'");
      return false;
    } else {
      websocket_port = 9500;
    }

    if (websocket["tls"]) websocket_tls = websocket["tls"].as<bool>();

    if (websocket["cert_pem_filename"]) websocket_cert_pem_filename = websocket["cert_pem_filename"].as<std::string>();
    if (websocket["key_pem_filename"]) websocket_key_pem_filename = websocket["key_pem_filename"].as<std::string>();

    // A listener asked to be secure with nothing to be secure with cannot start, and
    // falling back to ws:// would be a node quietly serving a browser in the clear.
    if (websocket_tls && (websocket_cert_pem_filename.empty() || websocket_key_pem_filename.empty())) {
      _logger->error("'websocket.tls' is set but 'websocket.cert_pem_filename' or 'websocket.key_pem_filename' is missing");
      return false;
    }
  }

  // --- Parse the 'datastore' section ---
  YAML::Node datastore = config["datastore"];
  if (datastore) {
    if (datastore["url"]) {
      db_url = datastore["url"].as<std::string>();
    } else {
      _logger->error("Missing 'datastore.url'");
      return false;
    }
  }

  // --- Parse the 'event' section ---
  YAML::Node events = config["events"];
  if (events) {
    if (events["url"]) {
      events_url = events["url"].as<std::string>();
    } else {
      _logger->error("Missing 'events.url'");
      return false;
    }

    if (events["status_interval"]) events_status_interval = events["status_interval"].as<std::uint32_t>();
  }

  // --- Parse the 'media' section ---
  YAML::Node media = config["media"];
  if (media) {
    if (media["url"]) media_url = media["url"].as<std::string>();
  }

  // --- Parse the 'http' section ---
  YAML::Node http = config["http"];
  if (http) {
    if (http["address"])
      http_address = http["address"].as<std::string>();
    else
      http_address = "0.0.0.0";

    if (http["port"]) {
      try {
        http_port = http["port"].as<uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'http.port': " + std::string(e.what()));
        return false;
      }
    } else {
      _logger->error("Missing 'http.port'");
      return false;
    }

    YAML::Node http_api = http["api"];
    if (http_api) {
      if (http_api["enable"])
        http_api_enable = http_api["enable"].as<bool>();
      else
        http_api_enable = true;

      // No tokens is not an open API: a request with no token matching gets 401, so an
      // API enabled without any is an API nobody can call. That is the safe way round.
      YAML::Node tokens = http_api["tokens"];
      if (tokens && tokens.IsSequence()) {
        for (const auto& entry : tokens) {
          ApiToken token;

          if (entry["token"]) token.token = entry["token"].as<std::string>();

          if (entry["scopes"] && entry["scopes"].IsSequence()) {
            for (const auto& scope : entry["scopes"]) token.scopes.push_back(scope.as<std::string>());
          }

          if (token.token.empty()) {
            _logger->error("Ignoring an 'http.api.tokens' entry with no token");
            continue;
          }

          http_api_tokens.push_back(std::move(token));
        }
      }

      if (http_api_enable && http_api_tokens.empty()) {
        _logger->warn("http.api.enable is set with no tokens: every request will be refused");
      }
    }

    YAML::Node http_files = http["files"];
    if (http_files) {
      if (http_files["enable"])
        http_files_enable = http_files["enable"].as<bool>();
      else
        http_files_enable = true;

      if (http_files["path"]) http_files_path = http_files["path"].as<std::string>();
      if (http_files["spa"]) http_files_spa = http_files["spa"].as<bool>();
    }
  }

  return true;
}

YAML::Node Config::plugin_root(const std::string& kind, const std::string& name) const {
  if (!_root || !_root[kind]) {
    return YAML::Node(YAML::NodeType::Undefined);
  }

  const YAML::Node section = _root[kind][name];
  if (!section) {
    return YAML::Node(YAML::NodeType::Undefined);
  }

  return section;
}

}  // namespace athenasip
