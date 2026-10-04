//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "util.h"

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

  // sip
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

  if (sip["allow_unencrypted"]) sip_allow_unencrypted = sip["allow_unencrypted"].as<bool>();

  if (sip["flow_idle_timeout"]) {
    try {
      sip_flow_idle_timeout = sip["flow_idle_timeout"].as<std::uint32_t>();
    } catch (const std::exception& e) {
      _logger->error("Invalid value for 'sip.flow_idle_timeout': " + std::string(e.what()));
      return false;
    }
  }
  if (sip["log_messages"]) sip_log_messages = sip["log_messages"].as<bool>();
  if (sip["public_address"]) sip_public_address = sip["public_address"].as<std::string>();

  if (sip["localnet"]) {
    try {
      sip_localnet = sip["localnet"].as<std::vector<std::string>>();
    } catch (const std::exception&) {
      _logger->error("Invalid 'sip.localnet': it is a list of prefixes, such as [\"192.168.0.0/16\"]");
      return false;
    }
  }
  if (!parse_localnet()) return false;
  if (sip["media_timeout"]) sip_media_timeout = sip["media_timeout"].as<uint32_t>();
  if (sip["max_call_duration"]) sip_max_call_duration = sip["max_call_duration"].as<uint32_t>();
  if (sip["require_session_timer"]) sip_require_session_timer = sip["require_session_timer"].as<bool>();

  if (sip["session_expires"]) {
    const auto configured = sip["session_expires"].as<uint32_t>();

    // RFC 4028 section 4: at least 90 seconds, and an inserted interval SHOULD NOT be under
    // 30 minutes. Zero means insert none.
    if (configured != 0 && configured < 90) {
      _logger->error("sip.session_expires must be at least 90 (RFC 4028) - keeping " + std::to_string(sip_session_expires));
    } else {
      if (configured != 0 && configured < 1800) _logger->warn("sip.session_expires below the 1800 RFC 4028 recommends for an inserted interval");
      sip_session_expires = configured;
    }
  }

  if (sip["session_min_se"]) {
    const auto configured = sip["session_min_se"].as<uint32_t>();

    // RFC 4028 section 8.1: the Min-SE a proxy quotes in a 422 MUST NOT be under 90 seconds.
    if (configured < 90) {
      _logger->error("sip.session_min_se must be at least 90 (RFC 4028) - keeping " + std::to_string(sip_session_min_se));
    } else {
      sip_session_min_se = configured;
    }
  }

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

      // RFC 3261 16.6 step 11: timer C MUST be larger than 3 minutes.
      if (configured <= 180000) {
        _logger->error("sip.timers.c_invite_proxy_ms must be larger than 180000 - keeping " + std::to_string(sip_timer_c_invite_proxy_ms));
      } else {
        sip_timer_c_invite_proxy_ms = configured;
      }
    }
  }

  if (sip["connect_timeout_ms"]) sip_connect_timeout_ms = sip["connect_timeout_ms"].as<uint32_t>();

  // tls
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
      // A port is required only when the listener is enabled. The same holds for tcp, udp
      // and websocket below.
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

  // tcp
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
      _logger->error("Missing 'tcp.port'");
      return false;
    } else {
      tcp_port = 5060;
    }
  }

  // udp
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
      _logger->error("Missing 'udp.port'");
      return false;
    } else {
      udp_port = 5060;
    }
  }

  // websocket
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
      _logger->error("Missing 'websocket.port'");
      return false;
    } else {
      websocket_port = 9500;
    }

    if (websocket["tls"]) websocket_tls = websocket["tls"].as<bool>();

    if (websocket["secure_port"]) {
      try {
        websocket_secure_port = websocket["secure_port"].as<std::uint16_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'websocket.secure_port': " + std::string(e.what()));
        return false;
      }
    }

    if (websocket_secure_port != 0 && websocket_secure_port == websocket_port) {
      _logger->error("'websocket.secure_port' is the same as 'websocket.port': the secure listener is a second one, on a port of its own");
      return false;
    }

    if (websocket["cert_pem_filename"]) websocket_cert_pem_filename = websocket["cert_pem_filename"].as<std::string>();
    if (websocket["key_pem_filename"]) websocket_key_pem_filename = websocket["key_pem_filename"].as<std::string>();

    // Never fall back to ws:// when wss was asked for.
    if (websocket_tls && (websocket_cert_pem_filename.empty() || websocket_key_pem_filename.empty())) {
      _logger->error("'websocket.tls' is set but 'websocket.cert_pem_filename' or 'websocket.key_pem_filename' is missing");
      return false;
    }
  }

  // datastore
  YAML::Node datastore = config["datastore"];
  if (datastore) {
    if (datastore["url"]) {
      db_url = datastore["url"].as<std::string>();
    } else {
      _logger->error("Missing 'datastore.url'");
      return false;
    }
  }

  // events
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

  // media
  YAML::Node media = config["media"];
  if (media) {
    if (media["url"]) media_url = media["url"].as<std::string>();
  }

  // public_port, for each listener.
  for (const auto& [name, field] : std::initializer_list<std::pair<const char*, std::uint16_t*>>{
           {"udp", &udp_public_port}, {"tcp", &tcp_public_port}, {"tls", &tls_public_port}, {"websocket", &websocket_public_port}}) {
    if (!config[name] || !config[name]["public_port"]) continue;
    try {
      *field = config[name]["public_port"].as<std::uint16_t>();
    } catch (const std::exception&) {
      _logger->error(std::string("Invalid '") + name + ".public_port': it is a port number");
      return false;
    }
  }

  // cluster
  if (YAML::Node cluster = config["cluster"]) {
    try {
      if (cluster["enable"]) cluster_enable = cluster["enable"].as<bool>();
      if (cluster["address"]) cluster_address = cluster["address"].as<std::string>();
      if (cluster["port"]) cluster_port = cluster["port"].as<std::uint16_t>();
      if (cluster["advertise"]) cluster_advertise = cluster["advertise"].as<std::string>();
      if (cluster["ca"]) cluster_ca = cluster["ca"].as<std::string>();
      if (cluster["cert"]) cluster_cert = cluster["cert"].as<std::string>();
      if (cluster["key"]) cluster_key = cluster["key"].as<std::string>();
    } catch (const std::exception& e) {
      _logger->error("Invalid 'cluster' section: " + std::string(e.what()));
      return false;
    }

    // The cluster listener is mutual TLS or nothing.
    if (cluster_enable && (cluster_ca.empty() || cluster_cert.empty() || cluster_key.empty())) {
      _logger->error("'cluster' needs ca, cert and key: athenasip --ca-init and --ca-node make them (docs/certificates.md)");
      return false;
    }
  }

  // behaviour. An unknown value stops the node rather than being ignored.
  YAML::Node behaviour_section = config["behaviour"];
  if (behaviour_section) {
    try {
      if (behaviour_section["media_anchor"]) behaviour.anchor = behaviour_section["media_anchor"].as<bool>();
    } catch (const std::exception& e) {
      _logger->error("Invalid value for 'behaviour.media_anchor': " + std::string(e.what()));
      return false;
    }

    if (behaviour_section["media_profile"]) {
      const auto named = behaviour_section["media_profile"].as<std::string>();
      const auto profiles = types::MediaPolicy::parse_profiles(named);
      if (!profiles) {
        _logger->error("Unknown 'behaviour.media_profile' '" + named + "': it is one of mirror, transport, webrtc, rtp or srtp");
        return false;
      }
      behaviour.profiles = *profiles;
    }

    if (behaviour_section["qualify_interval"]) {
      std::int64_t seconds = -1;
      try {
        seconds = behaviour_section["qualify_interval"].as<std::int64_t>();
      } catch (const std::exception&) {
      }

      if (!types::Behaviour::valid_qualify_interval(seconds)) {
        _logger->error("Invalid 'behaviour.qualify_interval': it is 0 for never, or seconds from " + std::to_string(types::Behaviour::kQualifyMinimum) +
                       " to " + std::to_string(types::Behaviour::kQualifyMaximum));
        return false;
      }
      behaviour_qualify_interval = static_cast<std::uint32_t>(seconds);
    }

    try {
      if (behaviour_section["rewrite_contact"]) behaviour_rewrite_contact = behaviour_section["rewrite_contact"].as<bool>();
    } catch (const std::exception&) {
      _logger->error("Invalid 'behaviour.rewrite_contact': it is true or false");
      return false;
    }
  }

  // log
  if (YAML::Node log = config["log"]) {
    try {
      if (log["level"]) {
        const auto level = Util::to_lower(log["level"].as<std::string>());
        if (level == "debug") {
          log_level = loggers::LogLevel::DEBUG;
        } else if (level == "info") {
          log_level = loggers::LogLevel::INFO;
        } else if (level == "warn") {
          log_level = loggers::LogLevel::WARN;
        } else if (level == "error") {
          log_level = loggers::LogLevel::ERROR;
        } else {
          _logger->error("Invalid 'log.level': " + level + " - it is debug, info, warn or error");
          return false;
        }
      }

      if (log["format"]) {
        const auto format = Util::to_lower(log["format"].as<std::string>());
        if (format == "text") {
          log_format = loggers::LogFormat::Text;
        } else if (format == "json") {
          log_format = loggers::LogFormat::Json;
        } else {
          _logger->error("Invalid 'log.format': " + format + " - it is text or json");
          return false;
        }
      }
    } catch (const std::exception& e) {
      _logger->error("Invalid 'log' section: " + std::string(e.what()));
      return false;
    }
  }

  // calls
  if (YAML::Node calls = config["calls"]) {
    try {
      if (calls["history_retention"]) calls_history_retention = calls["history_retention"].as<std::uint32_t>();
    } catch (const std::exception& e) {
      _logger->error("Invalid value for 'calls.history_retention': " + std::string(e.what()));
      return false;
    }
  }

  // http
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

    if (YAML::Node http_tls = http["tls"]) {
      try {
        if (http_tls["enable"]) http_tls_enable = http_tls["enable"].as<bool>();
        if (http_tls["address"]) http_tls_address = http_tls["address"].as<std::string>();
        if (http_tls["port"]) http_tls_port = http_tls["port"].as<std::uint16_t>();
        if (http_tls["cert_pem_filename"]) http_tls_cert_pem_filename = http_tls["cert_pem_filename"].as<std::string>();
        if (http_tls["key_pem_filename"]) http_tls_key_pem_filename = http_tls["key_pem_filename"].as<std::string>();
      } catch (const std::exception& e) {
        _logger->error("Invalid 'http.tls' section: " + std::string(e.what()));
        return false;
      }
    }

    if (http_tls_address.empty()) http_tls_address = http_address;

    if (http_tls_enable && (http_tls_cert().empty() || http_tls_key().empty())) {
      _logger->error("'http.tls' needs a certificate and a key: cert_pem_filename and key_pem_filename here, or in the 'tls' section");
      return false;
    }

    YAML::Node http_api = http["api"];
    if (http_api) {
      if (http_api["enable"])
        http_api_enable = http_api["enable"].as<bool>();
      else
        http_api_enable = true;

      // Configured API tokens are not supported. A file that sets them is refused, not
      // ignored, so the operator learns why they cannot sign in.
      if (http_api["tokens"]) {
        _logger->error(
            "'http.api.tokens' is no longer supported: configured API tokens were removed. Delete it, create an administrator with "
            "`athenasip --add-user NAME` on this host, and sign in as that user");
        return false;
      }

      try {
        if (http_api["session_lifetime"]) http_api_session_lifetime = http_api["session_lifetime"].as<std::uint32_t>();
        if (http_api["session_idle"]) http_api_session_idle = http_api["session_idle"].as<std::uint32_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'http.api.session_lifetime' or 'http.api.session_idle': " + std::string(e.what()));
        return false;
      }

      if (YAML::Node limits = http_api["rate_limits"]) {
        const auto read = [&limits](const char* name, RateLimit& into) {
          YAML::Node limit = limits[name];
          if (!limit) return;
          if (limit["burst"]) into.burst = limit["burst"].as<std::uint32_t>();
          if (limit["per_minute"]) into.per_minute = limit["per_minute"].as<std::uint32_t>();
        };

        try {
          read("open", http_api_limit_open);
          read("login_source", http_api_limit_login_source);
          read("login_user", http_api_limit_login_user);
          read("session", http_api_limit_session);
        } catch (const std::exception& e) {
          _logger->error("Invalid 'http.api.rate_limits': each of open, login_source, login_user and session takes burst and per_minute - " +
                         std::string(e.what()));
          return false;
        }
      }

      if (http_api_session_lifetime == 0) {
        _logger->error("'http.api.session_lifetime' cannot be zero: a session the datastore cannot expire is not one it can hold");
        return false;
      }

      // Only a warning: the session still ends, at its absolute lifetime.
      if (http_api_session_idle > http_api_session_lifetime) {
        _logger->warn("'http.api.session_idle' is longer than 'http.api.session_lifetime', so nothing will ever expire on idle");
      }

      YAML::Node ice = http_api["ice_servers"];
      if (ice && ice.IsSequence()) {
        for (const auto& entry : ice) {
          IceServer server;

          if (entry.IsScalar())
            server.url = entry.as<std::string>();
          else if (entry["url"])
            server.url = entry["url"].as<std::string>();

          if (server.url.empty()) {
            _logger->error("Ignoring an 'http.api.ice_servers' entry with no url");
            continue;
          }

          ice_servers.push_back(std::move(server));
        }
      }

      if (http_api["turn_shared_secret"]) turn_shared_secret = http_api["turn_shared_secret"].as<std::string>();

      try {
        if (http_api["turn_credential_ttl"]) turn_credential_ttl = http_api["turn_credential_ttl"].as<std::uint32_t>();
      } catch (const std::exception& e) {
        _logger->error("Invalid value for 'http.api.turn_credential_ttl': " + std::string(e.what()));
        return false;
      }

      if (turn_credential_ttl == 0) {
        _logger->error("'http.api.turn_credential_ttl' cannot be zero: a credential that has already expired is not one a client can use");
        return false;
      }

      // A browser cannot use a turn: URL without a credential, so warn at startup.
      const auto wants_turn = std::any_of(ice_servers.begin(), ice_servers.end(),
                                          [](const IceServer& server) { return server.url.rfind("turn:", 0) == 0 || server.url.rfind("turns:", 0) == 0; });

      if (wants_turn && turn_shared_secret.empty()) {
        _logger->warn("'http.api.ice_servers' names a TURN server with no 'http.api.turn_shared_secret', so clients get a URL they cannot authenticate to");
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

namespace {

// Copies a kind's plugin sections through from the document, skipping the keys in `mine`,
// which the server has already emitted from its own fields.
void emit_plugin_sections(YAML::Emitter& out, const YAML::Node& root, const std::string& kind, const std::vector<std::string>& mine) {
  if (!root || !root[kind] || !root[kind].IsMap()) return;

  for (const auto& entry : root[kind]) {
    const auto name = entry.first.as<std::string>();

    if (std::find(mine.begin(), mine.end(), name) != mine.end()) continue;

    out << YAML::Key << name << YAML::Value << entry.second;
  }
}

}  // namespace

std::string Config::effective_yaml() const {
  YAML::Emitter out;

  out << YAML::BeginMap;

  out << YAML::Key << "sip" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "node_id" << YAML::Value << sip_node_id;
  out << YAML::Key << "public_address" << YAML::Value << sip_public_address;
  out << YAML::Key << "localnet" << YAML::Value << YAML::Flow << sip_localnet;
  out << YAML::Key << "allow_unencrypted" << YAML::Value << sip_allow_unencrypted;
  out << YAML::Key << "log_messages" << YAML::Value << sip_log_messages;
  out << YAML::Key << "session_expires" << YAML::Value << sip_session_expires;
  out << YAML::Key << "session_min_se" << YAML::Value << sip_session_min_se;
  out << YAML::Key << "require_session_timer" << YAML::Value << sip_require_session_timer;
  out << YAML::Key << "max_call_duration" << YAML::Value << sip_max_call_duration;
  out << YAML::Key << "media_timeout" << YAML::Value << sip_media_timeout;
  out << YAML::Key << "connect_timeout_ms" << YAML::Value << sip_connect_timeout_ms;
  out << YAML::Key << "flow_idle_timeout" << YAML::Value << sip_flow_idle_timeout;

  out << YAML::Key << "timer_t1_rtt_ms" << YAML::Value << sip_timer_t1_rtt_ms;
  out << YAML::Key << "timer_t2_max_retransmit_interval_ms" << YAML::Value << sip_timer_t2_max_retransmit_interval_ms;
  out << YAML::Key << "timer_t4_network_propagation_ms" << YAML::Value << sip_timer_t4_network_propagation_ms;
  out << YAML::Key << "timer_b_invite_timeout" << YAML::Value << sip_timer_b_invite_timeout;
  out << YAML::Key << "timer_c_invite_proxy_ms" << YAML::Value << sip_timer_c_invite_proxy_ms;
  out << YAML::Key << "timer_f_non_invite_timeout" << YAML::Value << sip_timer_f_non_invite_timeout;
  out << YAML::Key << "timer_reliable_transport_retransmits" << YAML::Value << sip_timer_reliable_transport_retransmits;
  out << YAML::EndMap;

  const auto listener = [&out](const std::string& name, bool enable, const std::string& address, std::uint16_t port, std::uint16_t public_port) {
    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enable" << YAML::Value << enable;
    out << YAML::Key << "address" << YAML::Value << address;
    out << YAML::Key << "port" << YAML::Value << port;
    out << YAML::Key << "public_port" << YAML::Value << public_port;
  };

  listener("udp", udp_enable, udp_address, udp_port, udp_public_port);
  out << YAML::EndMap;

  listener("tcp", tcp_enable, tcp_address, tcp_port, tcp_public_port);
  out << YAML::EndMap;

  listener("tls", tls_enable, tls_address, tls_port, tls_public_port);
  out << YAML::Key << "cert_pem_filename" << YAML::Value << tls_cert_pem_filename;
  out << YAML::Key << "key_pem_filename" << YAML::Value << tls_key_pem_filename;
  out << YAML::EndMap;

  listener("websocket", websocket_enable, websocket_address, websocket_port, websocket_public_port);
  out << YAML::Key << "tls" << YAML::Value << websocket_tls;
  out << YAML::Key << "secure_port" << YAML::Value << websocket_secure_port;
  out << YAML::Key << "cert_pem_filename" << YAML::Value << websocket_cert_pem_filename;
  out << YAML::Key << "key_pem_filename" << YAML::Value << websocket_key_pem_filename;
  out << YAML::EndMap;

  out << YAML::Key << "datastore" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "url" << YAML::Value << db_url;
  emit_plugin_sections(out, _root, "datastore", {"url"});
  out << YAML::EndMap;

  out << YAML::Key << "events" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "url" << YAML::Value << events_url;
  out << YAML::Key << "status_interval" << YAML::Value << events_status_interval;
  emit_plugin_sections(out, _root, "events", {"url", "status_interval"});
  out << YAML::EndMap;

  out << YAML::Key << "media" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "url" << YAML::Value << media_url;
  emit_plugin_sections(out, _root, "media", {"url"});
  out << YAML::EndMap;

  out << YAML::Key << "cluster" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "enable" << YAML::Value << cluster_enable;
  out << YAML::Key << "address" << YAML::Value << cluster_address;
  out << YAML::Key << "port" << YAML::Value << cluster_port;
  out << YAML::Key << "advertise" << YAML::Value << cluster_advertise;
  out << YAML::Key << "ca" << YAML::Value << cluster_ca;
  out << YAML::Key << "cert" << YAML::Value << cluster_cert;
  out << YAML::Key << "key" << YAML::Value << cluster_key;
  out << YAML::EndMap;

  out << YAML::Key << "behaviour" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "media_anchor" << YAML::Value << behaviour.anchor;
  out << YAML::Key << "media_profile" << YAML::Value << types::MediaPolicy::to_string(behaviour.profiles);
  out << YAML::Key << "qualify_interval" << YAML::Value << behaviour_qualify_interval;
  out << YAML::Key << "rewrite_contact" << YAML::Value << behaviour_rewrite_contact;
  out << YAML::EndMap;

  out << YAML::Key << "log" << YAML::Value << YAML::BeginMap;
  const char* levels[] = {"debug", "info", "warn", "error"};
  out << YAML::Key << "level" << YAML::Value << levels[static_cast<int>(log_level)];
  out << YAML::Key << "format" << YAML::Value << (log_format == loggers::LogFormat::Json ? "json" : "text");
  out << YAML::EndMap;

  out << YAML::Key << "calls" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "history_retention" << YAML::Value << calls_history_retention;
  out << YAML::EndMap;

  out << YAML::Key << "http" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "address" << YAML::Value << http_address;
  out << YAML::Key << "port" << YAML::Value << http_port;

  out << YAML::Key << "tls" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "enable" << YAML::Value << http_tls_enable;
  out << YAML::Key << "address" << YAML::Value << http_tls_address;
  out << YAML::Key << "port" << YAML::Value << http_tls_port;
  out << YAML::Key << "cert_pem_filename" << YAML::Value << http_tls_cert();
  out << YAML::Key << "key_pem_filename" << YAML::Value << http_tls_key();
  out << YAML::EndMap;

  out << YAML::Key << "api" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "enable" << YAML::Value << http_api_enable;
  out << YAML::Key << "session_lifetime" << YAML::Value << http_api_session_lifetime;
  out << YAML::Key << "session_idle" << YAML::Value << http_api_session_idle;

  out << YAML::Key << "rate_limits" << YAML::Value << YAML::BeginMap;
  const auto emit_limit = [&out](const char* name, const RateLimit& limit) {
    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "burst" << YAML::Value << limit.burst;
    out << YAML::Key << "per_minute" << YAML::Value << limit.per_minute;
    out << YAML::EndMap;
  };
  emit_limit("open", http_api_limit_open);
  emit_limit("login_source", http_api_limit_login_source);
  emit_limit("login_user", http_api_limit_login_user);
  emit_limit("session", http_api_limit_session);
  out << YAML::EndMap;

  out << YAML::EndMap;

  out << YAML::Key << "files" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "enable" << YAML::Value << http_files_enable;
  out << YAML::Key << "path" << YAML::Value << http_files_path;
  out << YAML::Key << "spa" << YAML::Value << http_files_spa;
  out << YAML::EndMap;

  out << YAML::EndMap;
  out << YAML::EndMap;

  return std::string(out.c_str());
}

std::string Config::AdvertisedTransport::uri() const {
  return std::string(secure ? "sips:" : "sip:") + address + ":" + std::to_string(port) + ";transport=" + transport;
}

std::vector<Config::AdvertisedTransport> Config::advertised_transports() const {
  std::vector<AdvertisedTransport> out;

  const auto add = [this, &out](const std::string& transport, const std::string& bind_address, std::uint16_t port, bool secure) {
    const auto public_port = public_port_for(transport);
    out.push_back(AdvertisedTransport{transport, sip_public_address.empty() ? bind_address : sip_public_address,
                                      !sip_public_address.empty() && public_port != 0 ? public_port : port, secure});
  };

  if (udp_enable) add("udp", udp_address, udp_port, false);
  if (tcp_enable) add("tcp", tcp_address, tcp_port, false);
  if (tls_enable) add("tls", tls_address, tls_port, true);
  if (websocket_enable) add(websocket_tls ? "wss" : "ws", websocket_address, websocket_port, websocket_tls);

  // The secure_port listener. websocket.public_port applies only to the plain one.
  if (websocket_enable && !websocket_tls && websocket_secure_port != 0) {
    out.push_back(AdvertisedTransport{"wss", sip_public_address.empty() ? websocket_address : sip_public_address, websocket_secure_port, true});
  }

  return out;
}

std::optional<Config::AdvertisedTransport> Config::advertised_cluster() const {
  if (!cluster_enable) return std::nullopt;

  const bool everywhere = cluster_address.empty() || cluster_address == "0.0.0.0" || cluster_address == "::";

  std::string address = cluster_advertise;
  if (address.empty()) address = everywhere && !sip_public_address.empty() ? sip_public_address : cluster_address;

  return AdvertisedTransport{"tls", address, cluster_port, true};
}

// Each entry is a prefix or a bare address. Host bits are masked off, so 192.168.1.2/24
// means 192.168.1.0/24.
bool Config::parse_localnet() {
  _localnet_v4.clear();
  _localnet_v6.clear();

  for (const auto& entry : sip_localnet) {
    boost::system::error_code error;
    const auto slash = entry.find('/');

    if (slash == std::string::npos) {
      const auto address = boost::asio::ip::make_address(entry, error);
      if (!error && address.is_v4()) _localnet_v4.emplace_back(address.to_v4(), 32);
      if (!error && address.is_v6()) _localnet_v6.emplace_back(address.to_v6(), 128);
    } else if (entry.find(':') == std::string::npos) {
      const auto network = boost::asio::ip::make_network_v4(entry, error);
      if (!error) _localnet_v4.push_back(network.canonical());
    } else {
      const auto network = boost::asio::ip::make_network_v6(entry, error);
      if (!error) _localnet_v6.push_back(network.canonical());
    }

    if (error) {
      if (_logger) _logger->error("Invalid 'sip.localnet' entry '" + entry + "': it is a prefix such as 192.168.0.0/16, or an address");
      return false;
    }
  }

  return true;
}

bool Config::in_localnet(const boost::asio::ip::address& address) const {
  if (address.is_v4()) {
    const auto v4 = address.to_v4();
    for (const auto& network : _localnet_v4) {
      if (boost::asio::ip::make_network_v4(v4, network.prefix_length()).canonical() == network) return true;
    }
    return false;
  }

  const auto v6 = address.to_v6();
  if (v6.is_v4_mapped()) return in_localnet(boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6));

  for (const auto& network : _localnet_v6) {
    if (boost::asio::ip::make_network_v6(v6, network.prefix_length()).canonical() == network) return true;
  }
  return false;
}

std::uint16_t Config::public_port_for(const std::string& transport) const {
  if (transport == "udp") return udp_public_port;
  if (transport == "tcp") return tcp_public_port;
  if (transport == "tls") return tls_public_port;
  if (transport == "ws" || transport == "wss") return websocket_public_port;
  return 0;
}

}  // namespace athenasip
