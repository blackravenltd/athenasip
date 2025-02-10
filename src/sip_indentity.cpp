/*
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
*/
#include <regex>
#include <sstream>

#include "sip_identity.h"
#include "sip_uri.h"

namespace athenasip {

// Default constructor
SIPIdentity::SIPIdentity() : _uri() {}

// Constructor with parsing
SIPIdentity::SIPIdentity(const std::string& identity) { parse(identity); }

// Accessors
std::optional<std::string> SIPIdentity::display_name() const { return _display_name; }

const SIPUri& SIPIdentity::uri() const { return _uri; }

std::string SIPIdentity::to_string() const {
  std::ostringstream oss;
  if (_display_name) {
    oss << *_display_name << " ";
  }
  oss << "<" << _uri.to_string() << ">";
  return oss.str();
}

// Mutators
void SIPIdentity::set_display_name(const std::string& name) { _display_name = name; }

void SIPIdentity::set_uri(const SIPUri& uri) { _uri = uri; }

void SIPIdentity::clear_display_name() { _display_name.reset(); }

// Private parsing function
void SIPIdentity::parse(const std::string& identity) {
  static const std::regex sip_regex(R"(^\s*\"?([^\"<]+)\"?\s*<([^>]+)>\s*$)");
  std::smatch match;

  if (std::regex_match(identity, match, sip_regex)) {
    _display_name = match[1].matched ? std::optional<std::string>(Util::trim(match[1].str())) : std::nullopt;
    _uri = SIPUri(match[2].str());
  } else {
    _display_name.reset();
    _uri = SIPUri(identity);
  }
}

std::string operator+(const SIPIdentity& identity, const std::string& str) { return identity.to_string() + str; }
std::string operator+(const std::string& str, const SIPIdentity& identity) { return str + identity.to_string(); }

}  // namespace athenasip
