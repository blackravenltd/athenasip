//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_uri.h"

#include <regex>
#include <sstream>

namespace athenasip::types {

// Default constructor: initializes with default values.
SIPUri::SIPUri() {
  scheme = "sip";
  user = "";
  password = std::nullopt;
  realm = "";
  port = std::nullopt;
  parameters = "";
  headers = "";
  valid = false;
}

// Constructor with parsing: calls the parse function.
SIPUri::SIPUri(const std::string& uri) { parse(uri); }

// Converts the SIPUri back into a string.
std::string SIPUri::to_string() const {
  std::ostringstream oss;
  oss << scheme << ":";
  if (!user.empty()) {
    oss << user;
    if (password.has_value()) {
      oss << ":" << *password;
    }
    oss << "@";
  }
  oss << realm;
  if (port.has_value()) {
    oss << ":" << *port;
  }
  if (!parameters.empty()) {
    oss << ";" << parameters;
  }
  if (!headers.empty()) {
    oss << "?" << headers;
  }
  return oss.str();
}

namespace {

// RFC 3261 19.1.1 port is 1*DIGIT, and a port is 16 bits. An overlong run of digits is
// not a valid port, and must not throw out of a message parser.
bool parse_port(const std::string& text, uint16_t& out) {
  if (text.empty() || text.size() > 5) return false;

  unsigned long value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = (value * 10) + static_cast<unsigned long>(c - '0');
  }

  if (value == 0 || value > 65535) return false;

  out = static_cast<uint16_t>(value);
  return true;
}

}  // namespace

// Private parsing function using a regular expression.
void SIPUri::parse(const std::string& uri) {
  // The host is either an IPv6 reference in square brackets (RFC 3261 19.1.1, RFC 5118
  // section 4) or a run of characters up to the port, parameters or headers. The
  // brackets matter: the colons inside an IPv6 address are not a port separator.
  static const std::regex uri_regex(R"((sip|sips):(?:([^:@]+)(?::([^@]+))?@)?(\[[0-9A-Fa-f:.]+\]|[^:;?]+)(?::(\d+))?(;[^?]*)?(\?.*)?)");

  std::smatch match;
  if (std::regex_match(uri, match, uri_regex)) {
    uint16_t parsed_port = 0;

    if (match[5].matched && !parse_port(match[5].str(), parsed_port)) {
      // A host that carries an unusable port is not a usable URI.
      scheme = "sip";
      user.clear();
      password.reset();
      realm = uri;
      port.reset();
      parameters.clear();
      headers.clear();
      valid = false;
      return;
    }

    scheme = match[1].str();
    user = match[2].matched ? match[2].str() : "";
    password = match[3].matched ? std::make_optional(match[3].str()) : std::nullopt;
    realm = match[4].str();
    port = match[5].matched ? std::optional<uint16_t>(parsed_port) : std::nullopt;
    // Remove the leading ';' from parameters (if present)
    parameters = match[6].matched ? match[6].str().substr(1) : "";
    // Remove the leading '?' from headers (if present)
    headers = match[7].matched ? match[7].str().substr(1) : "";
    valid = true;
  } else {
    // Fallback: treat the entire input as the realm.
    scheme = "sip";
    user.clear();
    password.reset();
    realm = uri;
    port.reset();
    parameters.clear();
    headers.clear();
    valid = false;
  }
}

// Friend operators for concatenation.
std::string operator+(const SIPUri& uri, const std::string& str) { return uri.to_string() + str; }

std::string operator+(const std::string& str, const SIPUri& uri) { return str + uri.to_string(); }

}  // namespace athenasip::types
