//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::siptypes {

class SIPUri {
 public:
  // Constructors
  SIPUri();
  explicit SIPUri(const std::string& uri);

  // Public URI properties
  std::string scheme;
  std::string user;
  std::optional<std::string> password;
  std::string realm;
  std::optional<uint16_t> port;
  std::string parameters;
  std::string headers;

  // Indicates whether the URI was parsed successfully.
  bool valid;

  // Instead of accessors, use these public fields directly.
  std::string to_string() const;

  friend std::string operator+(const SIPUri& uri, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPUri& uri);

 private:
  void parse(const std::string& identity);
};

}  // namespace athenasip::siptypes
