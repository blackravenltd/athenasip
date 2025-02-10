//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <regex>
#include <sstream>

#include "sip_identity.h"

namespace athenasip {

// Default constructor
SIPUri::SIPUri() : _scheme("sip"), _user(""), _realm(""), _port(std::nullopt), _password(std::nullopt) {}

// Constructor with parsing
SIPUri::SIPUri(const std::string& uri) { parse(uri); }

// Accessors
std::string SIPUri::scheme() const { return _scheme; }

std::optional<std::string> SIPUri::user() const { return _user.empty() ? std::nullopt : std::optional<std::string>(_user); }

std::optional<std::string> SIPUri::password() const { return _password; }

std::string SIPUri::realm() const { return _realm; }

std::optional<uint16_t> SIPUri::port() const { return _port; }

std::optional<std::string> SIPUri::parameters() const { return _parameters.empty() ? std::nullopt : std::optional<std::string>(_parameters); }

std::optional<std::string> SIPUri::headers() const { return _headers.empty() ? std::nullopt : std::optional<std::string>(_headers); }

std::string SIPUri::to_string() const {
  std::ostringstream oss;

  oss << _scheme << ":";

  if (!_user.empty()) {
    oss << _user;
    if (_password) {
      oss << ":" << *_password;
    }
    oss << "@";
  }

  oss << _realm;

  if (_port) oss << ":" << *_port;
  if (!_parameters.empty()) oss << ";" << _parameters;
  if (!_headers.empty()) oss << "?" << _headers;

  return oss.str();
}

// Mutators
void SIPUri::set_scheme(const std::string& scheme) { _scheme = scheme; }

void SIPUri::set_user(const std::string& user) { _user = user; }

void SIPUri::set_password(const std::string& password) { _password = password; }

void SIPUri::set_realm(const std::string& realm) { _realm = realm; }

void SIPUri::set_port(uint16_t port) { _port = port; }

void SIPUri::set_parameters(const std::string& parameters) {
  _parameters = parameters.empty() ? "" : (parameters[0] == ';' ? parameters.substr(1) : parameters);
}
void SIPUri::set_headers(const std::string& headers) { _headers = headers.empty() ? "" : (headers[0] == '?' ? headers.substr(1) : headers); }

// Private parsing function
void SIPUri::parse(const std::string& uri) {
  static const std::regex uri_regex(R"((sip|sips):(?:([^:@]+)(?::([^@]+))?@)?([^:;?]+)(?::(\d+))?(;[^?]*)?(\?.*)?)");

  std::smatch match;

  if (std::regex_match(uri, match, uri_regex)) {
    _scheme = match[1].str();
    _user = match[2].matched ? match[2].str() : "";
    _password = match[3].matched ? std::make_optional(match[3].str()) : std::nullopt;
    _realm = match[4].str();
    _port = match[5].matched ? std::optional<uint16_t>(std::stoi(match[5].str())) : std::nullopt;
    _parameters = match[6].matched ? match[6].str().substr(1) : "";  // Remove leading ";"
    _headers = match[7].matched ? match[7].str().substr(1) : "";     // Remove leading "?"
    _valid = true;
  } else {
    _scheme = "sip";
    _user.clear();
    _password.reset();
    _realm = uri;
    _port.reset();
    _parameters.clear();
    _headers.clear();
    _valid = false;
  }
}

std::string operator+(const SIPUri& uri, const std::string& str) { return uri.to_string() + str; }
std::string operator+(const std::string& str, const SIPUri& uri) { return str + uri.to_string(); }

}  // namespace athenasip
