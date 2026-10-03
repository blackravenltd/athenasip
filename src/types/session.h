//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <ctime>
#include <string>

namespace athenasip::types {

// What a user holds after logging in.
//
// The token itself is never stored. What is stored is its SHA-256, so a dump of the
// datastore does not hand over live sessions; the token is 32 random bytes, so there is
// nothing to salt against and no dictionary to build. The caller hashes before it asks,
// and the store never sees the token at all.
struct Session {
  std::string token_hash;

  // Who this session is, by User::key(). The roles are not copied here: they are read
  // from the user on each request, so a role removed takes effect on the next request
  // rather than at the next login.
  std::string username;

  std::time_t created_at = 0;

  // The two ways a session ends by itself. Absolute is how long a login is good for at
  // all; idle is how long it survives without being used.
  std::time_t expires_at = 0;
  std::time_t last_seen_at = 0;

  bool has_expired(std::time_t now, std::time_t idle_timeout) const {
    if (expires_at != 0 && now >= expires_at) return true;
    if (idle_timeout > 0 && last_seen_at != 0 && now - last_seen_at >= idle_timeout) return true;

    return false;
  }
};

}  // namespace athenasip::types
