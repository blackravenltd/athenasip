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

// A logged-in user's session. Only the SHA-256 of the token is stored, so a datastore dump holds no live sessions.
// The token is 32 random bytes, so no salt is needed. The caller hashes before asking; the store never sees the token.
struct Session {
  std::string token_hash;

  // The user, by User::key(). Roles are read from the user on each request, so removing one takes effect at once.
  std::string username;

  std::time_t created_at = 0;

  // A session ends at an absolute expiry, or after going unused for the idle period.
  std::time_t expires_at = 0;
  std::time_t last_seen_at = 0;

  bool has_expired(std::time_t now, std::time_t idle_timeout) const {
    if (expires_at != 0 && now >= expires_at) return true;
    if (idle_timeout > 0 && last_seen_at != 0 && now - last_seen_at >= idle_timeout) return true;

    return false;
  }
};

}  // namespace athenasip::types
