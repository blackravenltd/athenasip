//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "util.h"

namespace athenasip {

std::string Util::to_hex(const std::vector<uint8_t>& vec) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto& num : vec) {
    oss << std::setw(2) << static_cast<unsigned int>(num);
  }
  return oss.str();
}

std::string Util::to_hex(const uint8_t arr[], uint16_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint16_t i = 0; i < len; i++) {
    // Cast to unsigned int to ensure numeric interpretation
    oss << std::setw(2) << static_cast<uint>(arr[i]);
  }
  return oss.str();
}

// Trim leading and trailing whitespace
std::string Util::trim(const std::string& str) {
  size_t first = str.find_first_not_of(" \t\r\n");
  size_t last = str.find_last_not_of(" \t\r\n");
  return (first == std::string::npos) ? "" : str.substr(first, last - first + 1);
}

}  // namespace athenasip