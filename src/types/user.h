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

// What a user may do. A role is a permission, not a rank: none implies another and there is no superuser. A user
// holds any combination, including none, which is what a new user starts with.
namespace roles {

inline constexpr const char* view_cluster_status = "view-cluster-status";
inline constexpr const char* manage_admin_users = "manage-admin-users";
inline constexpr const char* manage_realms = "manage-realms";
inline constexpr const char* manage_realm_subscribers = "manage-realm-subscribers";
inline constexpr const char* manage_cluster = "manage-cluster";
inline constexpr const char* manage_trunks = "manage-trunks";

// Every role, for validating a request.
inline std::vector<std::string> all() {
  return {view_cluster_status, manage_admin_users, manage_realms, manage_realm_subscribers, manage_cluster, manage_trunks};
}

inline bool is_known(const std::string& role) {
  const auto known = all();
  return std::find(known.begin(), known.end(), role) != known.end();
}

}  // namespace roles

// Someone who can use the API, and so the admin interface. Not a subscriber: a subscriber registers on a realm to
// make calls and authenticates with Digest; a user administers the server. Neither credential works as the other.
struct User {
  // Compared case-insensitively and stored as given: "Tom" and "tom" cannot both exist.
  std::string username;
  std::string display_name;

  // As produced by types::Password. The password itself is never held, logged or returned.
  std::string password_hash;

  std::vector<std::string> roles;

  // Disabled rather than deleted, so past actions keep a name. A disabled user cannot log in and their sessions are revoked.
  bool disabled = false;

  std::time_t created_at = 0;
  std::time_t last_login_at = 0;

  bool has_role(const std::string& role) const { return std::find(roles.begin(), roles.end(), role) != roles.end(); }

  // The key a user is stored and looked up under.
  static std::string normalise(const std::string& username) { return Util::to_lower(username); }

  std::string key() const { return normalise(username); }
};

}  // namespace athenasip::types
