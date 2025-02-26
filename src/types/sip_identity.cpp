//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_identity.h"

#include <sstream>
#include <optional>
#include <regex>
#include <unordered_map>
#include <iostream>  // For debugging, if needed

namespace athenasip::types {

SIPIdentity::SIPIdentity() 
    : wrapped(false), display_name(std::nullopt), uri(nullptr), tags() {}

SIPIdentity::SIPIdentity(const std::string& identity)
    : wrapped(false), display_name(std::nullopt), uri(nullptr), tags() {
  parse(identity);
}

void SIPIdentity::parse(const std::string& identity) {
  static const std::regex sip_regex(
      R"DELIM(^\s*(?:(?:<\s*sip:\s*(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;[^>]+)*\s*>)|(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;.*)?)\s*$)DELIM");
  std::smatch match;

  if (std::regex_match(identity, match, sip_regex)) {
    if (match[3].matched) {
      // Alternative A: Wrapped format was matched.
      wrapped = true;
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

  // Now extract parameters (tags) from the original string.
  size_t param_start = std::string::npos;
  size_t close_bracket = identity.find('>');
  if (close_bracket != std::string::npos) {
    param_start = identity.find(';', close_bracket);
  } else {
    param_start = identity.find(';');
  }
  if (param_start != std::string::npos) {
    std::string params_str = identity.substr(param_start);
    if (!params_str.empty() && params_str[0] == ';')
      params_str.erase(0, 1);

    std::istringstream iss(params_str);
    std::string token;
    while (std::getline(iss, token, ';')) {
      token = Util::trim(token);
      if (token.empty()) continue;
      size_t eq_pos = token.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = Util::trim(token.substr(0, eq_pos));
        std::string value = Util::trim(token.substr(eq_pos + 1));
        tags[key] = value;
      } else {
        tags[Util::trim(token)] = "";
      }
    }
  }
}

std::string SIPIdentity::to_string() const {
  std::ostringstream oss;
  if (display_name) {
    oss << "\"" << display_name.value() << "\" ";
  }
  oss << "<" << uri->to_string() << ">";
  for (const auto& [key, value] : tags) {
    oss << ";" << key;
    if (!value.empty())
      oss << "=" << value;
  }
  return oss.str();
}

std::string operator+(const SIPIdentity& identity, const std::string& str) {
  return identity.to_string() + str;
}

std::string operator+(const std::string& str, const SIPIdentity& identity) {
  return str + identity.to_string();
}

}  // namespace athenasip::types
