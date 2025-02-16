//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_uri.h"

#include <regex>
#include <sstream>

namespace athenasip::siptypes {

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

// Private parsing function using a regular expression.
void SIPUri::parse(const std::string& uri) {
  static const std::regex uri_regex(R"((sip|sips):(?:([^:@]+)(?::([^@]+))?@)?([^:;?]+)(?::(\d+))?(;[^?]*)?(\?.*)?)");

  std::smatch match;
  if (std::regex_match(uri, match, uri_regex)) {
    scheme = match[1].str();
    user = match[2].matched ? match[2].str() : "";
    password = match[3].matched ? std::make_optional(match[3].str()) : std::nullopt;
    realm = match[4].str();
    port = match[5].matched ? std::optional<uint16_t>(std::stoi(match[5].str())) : std::nullopt;
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

}  // namespace athenasip::siptypes
