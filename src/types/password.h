//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <string>

namespace athenasip::types {

// How a user's password is stored. A subscriber's credential is not this: that is HA1 (RFC 2617), on the Subscriber.
//
// PBKDF2-HMAC-SHA256 from OpenSSL. Argon2id would be better but is not in OpenSSL, and a memory-hard KDF is not
// something to hand-roll. The stored format is self-describing, so the algorithm or cost can change per user:
//
//   pbkdf2-sha256$<iterations>$<base64 salt>$<base64 hash>
class Password {
 public:
  // High enough to cost an attacker, low enough that a login is not a denial of service. Records keep the count
  // they were made with, so raising it does not invalidate them.
  static constexpr std::uint32_t default_iterations = 600000;

  // Empty for an empty password, which is refused rather than stored.
  static std::string hash(const std::string& password, std::uint32_t iterations = default_iterations);

  // False for anything this code did not write, including an empty stored value.
  static bool verify(const std::string& password, const std::string& stored);
};

}  // namespace athenasip::types
