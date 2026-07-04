//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::types {

class Realm {
 public:
  // Constructors
  Realm();
  explicit Realm(const std::string& name);

  uint64_t id;
  std::string name;
  std::string nonce_secret;
  uint32_t nonce_expiry = 3600;
  uint32_t registration_timeout = 5000;

  std::string to_string() const;
};

}  // namespace athenasip::types
