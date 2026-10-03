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

// A TURN credential that expires on its own, under the shared-secret scheme coturn calls
// `use-auth-secret`.
//
// The TURN server is told one secret and nothing else. It knows no users, has no database,
// and is never asked about anybody: it recomputes the HMAC from the username it was
// presented and accepts the credential if it matches and the expiry has not passed. That
// is what lets this node hand a browser a working relay without the two of them sharing
// anything but a string in a configuration file.
//
//   username = <unix expiry>[:<name>]
//   password = base64(HMAC-SHA1(secret, username))
//
// SHA-1 because the scheme says SHA-1 and the TURN server will compute SHA-1; it is a MAC
// under a secret rather than a signature over a document, which is the use SHA-1 has not
// been broken for. It is not a choice this code gets to make.
class TurnCredential {
 public:
  std::string username;
  std::string password;
  std::time_t expires_at = 0;

  // Empty username and password when there is no secret, which is how "TURN is not
  // configured" travels rather than a credential that cannot work.
  static TurnCredential issue(const std::string& secret, const std::string& name, std::time_t now, std::uint32_t ttl);
};

}  // namespace athenasip::types
