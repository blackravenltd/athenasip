//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <vector>

namespace athenasip {

class Util {
 public:
  static std::string to_hex(const std::vector<uint8_t>& vec);
  static std::string to_hex(const uint8_t arr[], uint16_t len);
  static std::string trim(const std::string& str);
};

}  // namespace athenasip