//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
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

std::string Util::md5(const std::string &input) {
    boost::uuids::detail::md5 hasher;
    boost::uuids::detail::md5::digest_type digest;
    hasher.process_bytes(input.data(), input.size());
    hasher.get_digest(digest);

    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0');
    // The digest consists of 4 uint32_t values.
    for (int i = 0; i < 4; ++i) {
        // Each value printed as 8 hex digits.
        oss << std::setw(8) << digest[i];
    }
    return oss.str();
}

}  // namespace athenasip