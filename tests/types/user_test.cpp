//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/user.h"

#include <gtest/gtest.h>

#include <string>

using athenasip::types::User;
namespace roles = athenasip::types::roles;

// There is no superuser, and nothing implies anything else. A user holds what it was
// given: any combination, including none.
TEST(UserTest, AUserHoldsOnlyTheRolesItWasGiven) {
  User user;
  user.roles = {roles::manage_realms};

  EXPECT_TRUE(user.has_role(roles::manage_realms));
  EXPECT_FALSE(user.has_role(roles::view_cluster_status));
  EXPECT_FALSE(user.has_role(roles::manage_admin_users));
  EXPECT_FALSE(user.has_role(roles::manage_realm_subscribers));
  EXPECT_FALSE(user.has_role(roles::manage_cluster));
}

// A new user has none, which is a useful state and the safe default: it can log in and
// do nothing until somebody decides what it is for.
TEST(UserTest, AUserWithNoRolesCanDoNothing) {
  const User user;

  for (const auto& role : roles::all()) EXPECT_FALSE(user.has_role(role)) << role;
}

TEST(UserTest, TheRolesAreTheFiveAndNothingElse) {
  EXPECT_EQ(roles::all().size(), 5u);

  EXPECT_TRUE(roles::is_known("view-cluster-status"));
  EXPECT_TRUE(roles::is_known("manage-admin-users"));
  EXPECT_TRUE(roles::is_known("manage-realms"));
  EXPECT_TRUE(roles::is_known("manage-realm-subscribers"));
  EXPECT_TRUE(roles::is_known("manage-cluster"));

  // The scopes this replaces, and a plausible typo, are not roles.
  EXPECT_FALSE(roles::is_known("admin"));
  EXPECT_FALSE(roles::is_known("client"));
  EXPECT_FALSE(roles::is_known("manage-realm-accounts"));
  EXPECT_FALSE(roles::is_known(""));
}

// Usernames are compared case-insensitively and kept as they were typed, so a display
// stays as somebody wrote it and two users cannot differ only by case.
TEST(UserTest, AUsernameIsMatchedWithoutRegardToCase) {
  User user;
  user.username = "Tom.Cully";

  EXPECT_EQ(user.key(), "tom.cully");
  EXPECT_EQ(user.username, "Tom.Cully");
  EXPECT_EQ(User::normalise("TOM.CULLY"), user.key());
}
