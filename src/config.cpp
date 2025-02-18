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

Config::Config(std::shared_ptr<Logger> logger) : _logger(logger), sip_port(0), sip_registration_timeout(0) {}

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

  if (sip["address"])
    sip_address = sip["address"].as<std::string>();
  else {
    _logger->error("Missing 'sip.address'");
    return false;
  }

  if (sip["port"]) {
    try {
      sip_port = sip["port"].as<uint16_t>();
    } catch (const std::exception &e) {
      _logger->error("Invalid value for 'sip.port': " + std::string(e.what()));
      return false;
    }
  } else {
    _logger->error("Missing 'sip.port'");
    return false;
  }

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
  if (!config["tls"]) {
    _logger->error("YAML file missing 'sip' section");
    return false;
  }
  YAML::Node tls = config["tls"];

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

  // --- Parse the 'db' section ---
  if (config["db"] && config["db"]["url"])
    db_database_url = config["db"]["url"].as<std::string>();
  else {
    _logger->error("Missing 'db.url'");
    return false;
  }

  return true;
}

}  // namespace athenasip
