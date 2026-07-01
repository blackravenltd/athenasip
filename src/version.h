//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace athenasip {

class Version {
 public:
  Version(uint8_t* ptr, size_t len);
  Version(const std::string& version);
  Version(uint8_t pmajor, uint8_t pminor, uint8_t ppatch);

  size_t pack(uint8_t* ptr);
  std::string to_string();

  friend std::ostream& operator<<(std::ostream& os, const Version& v);

  uint8_t major;
  uint8_t minor;
  uint8_t patch;
};

}  // namespace athenasip