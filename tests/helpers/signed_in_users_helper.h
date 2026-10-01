//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <ctime>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "api/sessions.h"
#include "sync_datastore_helper.h"
#include "types/password.h"
#include "types/user.h"

// Users signed in through the real Sessions, for the API tests. Configured tokens went on
// 2026-10-01, so every credential a test presents is a session a user logged in for. The
// tests still name them the way they always did, and a fixture maps those names to tokens
// here:
//
//   admin-token      a user holding every role
//   client-token     a user holding view-cluster-status alone, what a client reads with
//   scopeless-token  a user holding no role at all
//
// Call after the API's executor is running: a login answers on it.
class SignedInUsers {
 public:
  SignedInUsers() = default;

  SignedInUsers(const std::shared_ptr<athenasip::api::Sessions>& sessions, SyncDatastore& store) {
    _sign_in(sessions, store, "admin-token", "test-admin", athenasip::types::roles::all());
    _sign_in(sessions, store, "client-token", "test-status", {athenasip::types::roles::view_cluster_status});
    _sign_in(sessions, store, "scopeless-token", "test-noroles", {});
  }

  // What to put after "Bearer ": the session for a name above, or the string itself, so a
  // test can still present a token that names nothing.
  std::string presented(const std::string& token) const {
    const auto found = _tokens.find(token);
    return found == _tokens.end() ? token : found->second;
  }

 private:
  void _sign_in(const std::shared_ptr<athenasip::api::Sessions>& sessions, SyncDatastore& store, const std::string& alias, const std::string& username,
                std::vector<std::string> roles) {
    auto user = std::make_shared<athenasip::types::User>();
    user->username = username;
    user->roles = std::move(roles);
    user->password_hash = athenasip::types::Password::hash("test-password", 1000);
    user->created_at = std::time(nullptr);
    store.user_create(user);

    std::promise<std::string> token;
    auto future = token.get_future();
    sessions->login(username, "test-password",
                    [&token](athenasip::api::Sessions::Login login) { token.set_value(login.ok() ? login.value.token : std::string()); });

    _tokens[alias] = future.get();
  }

  std::map<std::string, std::string> _tokens;
};
