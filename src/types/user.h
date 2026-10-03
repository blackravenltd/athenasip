//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "util.h"

namespace athenasip::types {

// What a user may do. There is no superuser: nothing here implies anything else, and a
// user holds the roles they were given and no others - any combination of them,
// including none, which is what a new user starts with.
//
// A role is a permission rather than a rank. Somebody who needs to read cluster status
// and manage realms is given both; there is deliberately no role that covers both.
namespace roles {

inline constexpr const char* view_cluster_status = "view-cluster-status";
inline constexpr const char* manage_admin_users = "manage-admin-users";
inline constexpr const char* manage_realms = "manage-realms";
inline constexpr const char* manage_realm_subscribers = "manage-realm-subscribers";
inline constexpr const char* manage_cluster = "manage-cluster";

// Every role there is, which is what validates a request.
inline std::vector<std::string> all() { return {view_cluster_status, manage_admin_users, manage_realms, manage_realm_subscribers, manage_cluster}; }

inline bool is_known(const std::string& role) {
  const auto known = all();
  return std::find(known.begin(), known.end(), role) != known.end();
}

}  // namespace roles

// A thing that can use the API, and therefore the admin interface, which is only a
// client of the API.
//
// Not a subscriber. A subscriber is registered on a realm to make and receive calls and
// authenticates with Digest against an HA1; a user administers the server those
// subscribers register to. Neither is created from the other in either direction, and
// neither credential works as the other.
struct User {
  // Compared case-insensitively and stored as given, the way an address is in practice:
  // "Tom" and "tom" cannot both exist.
  std::string username;
  std::string display_name;

  // As produced by types::Password. The password itself is never held, never logged and
  // never returned by any endpoint.
  std::string password_hash;

  std::vector<std::string> roles;

  // Kept rather than deleted, so what this user did still has a name against it. A
  // disabled user cannot log in and their sessions are revoked.
  bool disabled = false;

  std::time_t created_at = 0;
  std::time_t last_login_at = 0;

  bool has_role(const std::string& role) const { return std::find(roles.begin(), roles.end(), role) != roles.end(); }

  // The key a user is stored and looked up under.
  static std::string normalise(const std::string& username) { return Util::to_lower(username); }

  std::string key() const { return normalise(username); }
};

}  // namespace athenasip::types
