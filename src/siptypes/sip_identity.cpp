/*
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
*/
#include "sip_identity.h"

namespace athenasip::siptypes {

// Default constructor
SIPIdentity::SIPIdentity() : uri() {}

// Constructor with parsing
SIPIdentity::SIPIdentity(const std::string& identity) { parse(identity); }

std::string SIPIdentity::to_string() const {
  std::string out;
  if (display_name) out += display_name.value() + " ";
  out += "<" + uri.to_string() + ">";
  return out;
}

// Private parsing function
void SIPIdentity::parse(const std::string& identity) {
  static const std::regex sip_regex(R"(^\s*\"?([^\"<]+)\"?\s*<([^>]+)>\s*$)");
  std::smatch match;

  if (std::regex_match(identity, match, sip_regex)) {
    display_name = match[1].matched ? std::optional<std::string>(Util::trim(match[1].str())) : std::nullopt;
    uri = SIPUri(match[2].str());
  } else {
    display_name.reset();
    uri = SIPUri(identity);
  }
}

std::string operator+(const SIPIdentity& identity, const std::string& str) { return identity.to_string() + str; }
std::string operator+(const std::string& str, const SIPIdentity& identity) { return str + identity.to_string(); }

}  // namespace athenasip::siptypes
