//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../util.h"
#include "sip_uri.h"

namespace athenasip::types {

class SIPIdentity {
 public:
  // Constructors
  SIPIdentity() : uri(), tags() {}
  explicit SIPIdentity(const std::string& identity) : tags() { parse(identity); }

  bool wrapped;
  std::optional<std::string> display_name;
  std::shared_ptr<SIPUri> uri;
  std::unordered_map<std::string, std::string> tags;  // Stores parameters (e.g. tag, etc.)

  void parse(const std::string& identity) {
    static const std::regex sip_regex(
        R"DELIM(^\s*(?:(?:<\s*sip:\s*(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;[^>]+)*\s*>)|(?:(?:"([^"<]+)"|([^"<]+))\s*)?<\s*([^>]+)\s*>(?:;.*)?)\s*$)DELIM");

    std::smatch match;
    bool parsed = false;
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
      parsed = true;
    } else {
      // Fallback: treat the entire input as a SIP URI.
      wrapped = false;
      display_name.reset();
      uri = std::make_shared<SIPUri>(identity);
    }

    // Now extract parameters (tags) from the original string.
    // We assume parameters come after the SIP URI.
    size_t param_start = std::string::npos;
    // If the identity contains a '>' (i.e. in wrapped formats), search after it.
    size_t close_bracket = identity.find('>');
    if (close_bracket != std::string::npos) {
      param_start = identity.find(';', close_bracket);
    } else {
      // Otherwise, search for the first semicolon.
      param_start = identity.find(';');
    }
    if (param_start != std::string::npos) {
      // The substring from param_start onward should contain parameters.
      std::string params_str = identity.substr(param_start);
      // Remove the leading semicolon if present.
      if (!params_str.empty() && params_str[0] == ';') params_str.erase(0, 1);

      // Split by ';'
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
          // Parameter with no '='; store it with an empty string.
          tags[Util::trim(token)] = "";
        }
      }
    }
  }

  std::string to_string() const {
    std::ostringstream oss;
    if (display_name) oss << "\"" << display_name.value() << "\" ";
    oss << "<" << uri->to_string() << ">";
    // Append any parameters.
    for (const auto& [key, value] : tags) {
      oss << ";" << key;
      if (!value.empty()) oss << "=" << value;
    }
    return oss.str();
  }

  friend std::string operator+(const SIPIdentity& identity, const std::string& str) { return identity.to_string() + str; }
  friend std::string operator+(const std::string& str, const SIPIdentity& identity) { return str + identity.to_string(); }
};

}  // namespace athenasip::types
