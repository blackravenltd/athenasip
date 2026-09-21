//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace athenasip::types {

// A URL in the RFC 3986 generic syntax, as far as AthenaSIP reads one: every URL here
// names a driver and the address it talks to, so the hier-part is always an authority
// and a scheme with no "://" after it is not one of ours.
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
  bool is_valid() const;

  friend bool operator==(const URL& lhs, const URL& rhs);

  friend std::string operator+(const URL& url, const std::string& str);
  friend std::string operator+(const std::string& str, const URL& url);

 private:
  bool _valid{false};

  // The well-known port for a scheme, or nothing for a scheme that has none. RFC 3986
  // 3.1 makes the scheme case-insensitive, so the lookup is too.
  static std::optional<uint16_t> _default_port(const std::string& scheme);

  static const std::map<std::string, uint16_t> defaultPorts;
};

}  // namespace athenasip::types
