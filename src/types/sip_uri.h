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
// Parameters and headers are structured rather than kept as the raw text after the ';'
// and the '?'. The proxy has to read them: lr decides whether a Route is loose (16.12),
// transport and maddr decide where a request goes (16.6), and none of that is possible
// against an opaque string.
//
// Values are held unescaped. The escaping in 19.1.2 and 25.1 is a transport encoding,
// so it is undone on the way in and put back by to_string(); a caller comparing a user
// part should never have to think about it.
class SIPUri {
 public:
  SIPUri();
  explicit SIPUri(const std::string& uri);

  std::string scheme;
  std::string user;
  std::optional<std::string> password;

  // The host of the URI (19.1.1). Not a realm: a realm is the Digest protection domain
  // (22.1) and a different concept that happens to be spelled the same way.
  std::string host;

  std::optional<std::uint16_t> port;

  // Whether the URI parsed. An unparseable one keeps the original text in host so that
  // it can still be logged and compared, which is what "*" in a Contact relies on.
  bool valid;

  // Parameters and headers, in the order they were seen so that a URI we did not create
  // round-trips as it arrived. Names are matched case-insensitively (19.1.1) but kept
  // as they were written.
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

  // RFC 3261 19.1.4. Not operator==: this is the RFC's equivalence, which is neither
  // string equality nor a total order, and a reader should be made to notice that.
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
