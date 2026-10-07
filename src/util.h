//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace athenasip {

class Util {
 public:
  static std::string to_hex(const std::vector<uint8_t>& vec);
  static std::string to_hex(const uint8_t arr[], uint16_t len);
  static std::string to_upper(const std::string& str);
  static std::string to_lower(const std::string& str);
  static std::string trim(const std::string& str);
  static std::string trim(const std::string& str, const std::string& trimmable);
  static std::string md5(const std::string& input);

  // For RFC 8760 Digest; see digest.h.
  static std::string sha256(const std::string& input);
  static std::filesystem::path expand_path(const std::string& input);
  static bool is_ipv4(const std::string& ipv4);
  static bool is_ipv4_private(const std::string& ipv4);
  static std::string to_iso8601(const std::chrono::system_clock::time_point& tp);
  static std::string to_iso8601(const std::time_t& t);
  static std::size_t hash_combine(std::size_t seed, std::size_t hash_value);

  // A 64-bit FNV-1a id derived from a name, identical on every node. Bindings record their
  // subscriber's id, so it must not depend on which node provisioned the subscriber. Stable
  // and well spread, not unguessable.
  static std::uint64_t stable_id(const std::string& name);
  static std::string generate_random_string(const std::string& prefix, uint16_t length);
  static const std::string get_zulu_time();
};

}  // namespace athenasip