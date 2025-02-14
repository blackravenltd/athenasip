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
  out += "<" + uri->to_string() + ">";
  return out;
}

// Private parsing function
void SIPIdentity::parse(const std::string& identity) {
  // This regex has two alternatives:
  //  Alternative A (wrapped):
  //    Matches strings starting with "<sip:" and ending with ">"
  //    Captures:
  //      Group 1: quoted display name (if present)
  //      Group 2: unquoted display name (if present)
  //      Group 3: SIP URI (inside the inner angle brackets)
  //    It also allows trailing semicolon-parameters before the closing '>'
  //
  //  Alternative B (standard):
  //    Matches strings that do NOT start with the "<sip:" wrapper.
  //    Captures:
  //      Group 4: quoted display name (if present)
  //      Group 5: unquoted display name (if present)
  //      Group 6: SIP URI (inside angle brackets)
  //    It also allows trailing parameters after the closing '>'
  static const std::regex sip_regex(
    R"DELIM(^\s*(?:(?:<\s*sip:\s*(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;[^>]+)*\s*>)|(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;.*)?)\s*$)DELIM"
  );

  std::smatch match;
  if (std::regex_match(identity, match, sip_regex)) {
    if (match[3].matched) {
      // Alternative A: Wrapped format was matched.
      wrapped = true;
      // Use group 1 if quoted display name is present; otherwise group 2.
      std::string dn = match[1].matched ? match[1].str() : (match[2].matched ? match[2].str() : "");
      dn = Util::trim(dn);
      display_name = dn.empty() ? std::nullopt : std::optional<std::string>(dn);
      uri = std::make_shared<SIPUri>(match[3].str());
    } else {
      // Alternative B: Standard format was matched.
      wrapped = false;
      std::string dn = match[4].matched ? match[4].str() : (match[5].matched ? match[5].str() : "");
      dn = Util::trim(dn);
      display_name = dn.empty() ? std::nullopt : std::optional<std::string>(dn);
      uri = std::make_shared<SIPUri>(match[6].str());
    }
  } else {
    // Fallback: treat the entire input as a SIP URI.
    wrapped = false;
    display_name.reset();
    uri = std::make_shared<SIPUri>(identity);
  }
}


std::string operator+(const SIPIdentity& identity, const std::string& str) { return identity.to_string() + str; }
std::string operator+(const std::string& str, const SIPIdentity& identity) { return str + identity.to_string(); }

}  // namespace athenasip::siptypes
