//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/regex.hpp>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <string>

namespace athenasip::types {

class URL {
 public:
  std::string scheme;
  std::optional<std::string> username;
  std::optional<std::string> password;
  std::string host;
  std::optional<uint16_t> port;
  std::string path;
  std::string query;
  std::string fragment;

  URL();
  URL(const std::string& url);

  void parse(const std::string& url);
  std::string to_string() const;
  bool is_valid();

  friend bool operator==(const URL& lhs, const URL& rhs);

  friend std::string operator+(const URL& url, const std::string& str);
  friend std::string operator+(const std::string& str, const URL& url);

 private:
  bool _valid{false};

  bool isDefaultPort() const;
  std::string toLower(const std::string& input) const;

  static const std::map<std::string, uint16_t> defaultPorts;
};

}  // namespace athenasip::types
