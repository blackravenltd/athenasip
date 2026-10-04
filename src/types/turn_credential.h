//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace athenasip::types {

// A self-expiring TURN credential under the shared-secret scheme coturn calls `use-auth-secret`. The TURN server
// holds only the secret: it recomputes the HMAC from the username presented and checks the expiry.
//
//   username = <unix expiry>[:<name>]
//   password = base64(HMAC-SHA1(secret, username))
//
// SHA-1 is fixed by the scheme.
class TurnCredential {
 public:
  std::string username;
  std::string password;
  std::time_t expires_at = 0;

  // Empty username and password when there is no secret, meaning TURN is not configured.
  static TurnCredential issue(const std::string& secret, const std::string& name, std::time_t now, std::uint32_t ttl);
};

}  // namespace athenasip::types
