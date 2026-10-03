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

// How a user's password is stored, and the only place in this tree that turns one into
// something at rest. A subscriber's credential is not this: that is HA1, fixed by RFC
// 2617, and lives on the Subscriber.
//
// PBKDF2-HMAC-SHA256, from OpenSSL, which is already a dependency. Argon2id would be
// the better choice on its merits and is what a greenfield design should use; it is not
// in OpenSSL, and a memory-hard KDF is precisely the thing not to hand-roll. The stored
// format is self-describing so that decision can be revisited per user without
// invalidating anybody who has not logged in since.
//
//   pbkdf2-sha256$<iterations>$<base64 salt>$<base64 hash>
class Password {
 public:
  // High enough to cost an attacker and low enough that a login is not a denial of
  // service against the node doing it. Raise it over time; old records keep working
  // because the count they were made at is stored with them.
  static constexpr std::uint32_t default_iterations = 600000;

  // Empty for an empty password, which is refused rather than stored: a user that
  // anything logs into with nothing is not a user.
  static std::string hash(const std::string& password, std::uint32_t iterations = default_iterations);

  // False for anything this code did not write, including an empty stored value. A
  // verifier that accepted a malformed record would turn a corrupted row, or a column
  // somebody had emptied, into a way in.
  static bool verify(const std::string& password, const std::string& stored);
};

}  // namespace athenasip::types
