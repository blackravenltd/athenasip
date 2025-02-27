//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace athenasip {

Config::Config(std::shared_ptr<Logger> logger) : _logger(logger) {}

bool Config::load_from_yaml(const std::string &filename) {
  YAML::Node config;
  try {
    config = YAML::LoadFile(filename);
  } catch (const YAML::Exception &e) {
    _logger->error("Failed to parse YAML file: " + std::string(e.what()));
    return false;
  }

  // --- Parse the 'sip' section ---
  if (!config["sip"]) {
    _logger->error("YAML file missing 'sip' section");
    return false;
  }
  YAML::Node sip = config["sip"];

  if (sip["realm"])
    sip_realm = sip["realm"].as<std::string>();
  else {
    _logger->error("Missing 'sip.realm'");
    return false;
  }

  if (sip["nonce_secret"])
    sip_nonce_secret = sip["nonce_secret"].as<std::string>();
  else {
    _logger->error("Missing 'sip.nonce_secret'");
    return false;
  }

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
      } catch (const std::exception &e) {
        _logger->error("Invalid value for 'tls.port': " + std::string(e.what()));
        return false;
      }
    } else {
      _logger->error("Missing 'tls.port'");
      return false;
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
      } catch (const std::exception &e) {
        _logger->error("Invalid value for 'tcp.port': " + std::string(e.what()));
        return false;
      }
    } else {
      _logger->error("Missing 'tcp.port'");
      return false;
    }
  }

  // --- Parse the 'tls' section ---
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
      } catch (const std::exception &e) {
        _logger->error("Invalid value for 'udp.port': " + std::string(e.what()));
        return false;
      }
    } else {
      _logger->error("Missing 'udp.port'");
      return false;
    }
  }

  // --- Parse the 'db' section ---
  if (config["db"] && config["db"]["url"])
    db_database_url = config["db"]["url"].as<std::string>();
  else {
    _logger->error("Missing 'db.url'");
    return false;
  }

  // --- Parse the 'rtprelay' section ---
  YAML::Node rtprelay = config["rtprelay"];
  if (rtprelay) {
    if (rtprelay["enable"])
      rtprelay_enable = rtprelay["enable"].as<bool>();
    else
      rtprelay_enable = true;

    if (rtprelay["address"])
      rtprelay_address = rtprelay["address"].as<std::string>();
    else
      rtprelay_address = "0.0.0.0";

    if (rtprelay["public_address"])
      rtprelay_public_address = rtprelay["public_address"].as<std::string>();
    else
      rtprelay_public_address = "0.0.0.0";

    if (rtprelay["min_port"]) {
      try {
        rtprelay_min_port = rtprelay["min_port"].as<uint16_t>();
      } catch (const std::exception &e) {
        _logger->error("Invalid value for 'rtprelay.min_port': " + std::string(e.what()));
      }
    } else {
      rtprelay_min_port = 22000;
    }
    if (rtprelay["max_port"]) {
      try {
        rtprelay_max_port = rtprelay["max_port"].as<uint16_t>();
      } catch (const std::exception &e) {
        _logger->error("Invalid value for 'rtprelay.max_port': " + std::string(e.what()));
      }
    } else {
      rtprelay_max_port = 23000;
    }
  }

  return true;
}

}  // namespace athenasip
