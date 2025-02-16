//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "via_header.h"

namespace athenasip::headers {

bool ViaHeader::parse(const std::string& value) {
  // Reset members.
  version.clear();
  host.clear();
  parameters.clear();

  std::string s = Util::trim(value);
  // Expecting a format like:
  // "SIP/2.0/TLS client.example.com;branch=z9hG4bK776asdhds"
  // The first token (up to the space) is the version.
  auto space_pos = s.find(' ');
  if (space_pos == std::string::npos) {
    return false;  // Invalid format
  }
  version = s.substr(0, space_pos);
  std::string rest = Util::trim(s.substr(space_pos + 1));

  // The host is before the first semicolon (if any).
  auto semicolon_pos = rest.find(';');
  if (semicolon_pos == std::string::npos) {
    host = rest;
    return true;
  }
  host = Util::trim(rest.substr(0, semicolon_pos));
  std::string params_str = rest.substr(semicolon_pos + 1);

  // Parse parameters separated by semicolons.
  while (!params_str.empty()) {
    auto next_semicolon = params_str.find(';');
    std::string param;
    if (next_semicolon == std::string::npos) {
      param = params_str;
      params_str.clear();
    } else {
      param = params_str.substr(0, next_semicolon);
      params_str = params_str.substr(next_semicolon + 1);
    }
    param = Util::trim(param);
    if (param.empty()) {
      continue;
    }
    // Each parameter is in the form key[=value].
    auto equal_pos = param.find('=');
    if (equal_pos == std::string::npos) {
      parameters[param] = "";
    } else {
      std::string key = Util::trim(param.substr(0, equal_pos));
      std::string val = Util::trim(param.substr(equal_pos + 1));
      parameters[key] = val;
    }
  }
  return true;
}

std::string ViaHeader::to_string() const {
  std::ostringstream oss;
  oss << version << " " << host;
  for (const auto& param : parameters) {
    oss << ";" << param.first;
    if (!param.second.empty()) {
      oss << "=" << param.second;
    }
  }
  return oss.str();
}

}  // namespace athenasip::headers
