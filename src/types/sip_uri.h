//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace athenasip::types {

// RFC 3261 19.1: sip:user:password@host:port;uri-parameters?headers
//
// Parameters and headers are structured because the proxy reads them: lr decides whether a Route is loose (16.12),
// transport and maddr where a request goes (16.6).
//
// Values are held unescaped; the escaping of 19.1.2 and 25.1 is undone on the way in and put back by to_string().
class SIPUri {
 public:
  SIPUri();
  explicit SIPUri(const std::string& uri);

  std::string scheme;
  std::string user;
  std::optional<std::string> password;

  // The host of the URI (19.1.1). Not a realm, which is the Digest protection domain (22.1).
  std::string host;

  std::optional<std::uint16_t> port;

  // Whether the URI parsed. An unparseable one keeps the original text in host, so it can still be logged and compared.
  bool valid;

  // Parameters and headers in the order seen, so a URI round-trips as it arrived. Names are matched
  // case-insensitively (19.1.1) but kept as written.
  using Fields = std::vector<std::pair<std::string, std::string>>;

  const Fields& parameters() const { return _parameters; }
  const Fields& headers() const { return _headers; }

  bool has_parameter(std::string_view name) const;
  std::string parameter(std::string_view name) const;
  void set_parameter(std::string name, std::string value);
  void remove_parameter(std::string_view name);

  bool has_header(std::string_view name) const;
  std::string header(std::string_view name) const;
  void set_header(std::string name, std::string value);

  // RFC 3261 19.1.4 equivalence, which is neither string equality nor a total order, hence not operator==.
  bool equivalent_to(const SIPUri& other) const;

  std::string to_string() const;

  friend std::string operator+(const SIPUri& uri, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPUri& uri);

 private:
  void parse(const std::string& uri);
  void _reset_invalid(const std::string& uri);

  Fields _parameters;
  Fields _headers;
};

}  // namespace athenasip::types
