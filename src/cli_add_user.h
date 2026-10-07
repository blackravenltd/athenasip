//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "datastores/datastore.h"
#include "plugins/plugin.h"
#include "types/password.h"

namespace athenasip::cli {

// Creates an administrator without the API: how the first one is made, and the way back in
// when every admin password is lost or the HTTP listener is unreachable. Touches only the
// datastore, so it is safe to run beside a serving node.
struct AddUserResult {
  enum class Outcome {
    created,
    taken,         // a user of that name already exists
    unknown_role,  // a role this build does not have
    refused,       // the datastore said no, or cannot hold users at all
    invalid,       // nothing usable was given
  };

  Outcome outcome = Outcome::invalid;
  std::string message;

  bool ok() const { return outcome == Outcome::created; }
};

// Blocks on the async contract, which suits a command with nothing to serve. Roles are
// validated here because the datastore stores an unrecognised one rather than dropping it.
AddUserResult add_user(std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, const std::string& username,
                       const std::string& display_name, const std::vector<std::string>& roles, const std::string& password,
                       std::uint32_t iterations = types::Password::default_iterations);

// Gives an existing user a new password and ends every session it holds. Touches only the
// datastore.
struct ResetPasswordResult {
  enum class Outcome {
    reset,
    missing,  // no user of that name: making one is add_user's, with the roles it asks for
    refused,  // the datastore said no, or cannot hold users at all
    invalid,  // nothing usable was given
  };

  Outcome outcome = Outcome::invalid;
  std::string message;

  bool ok() const { return outcome == Outcome::reset; }
};

ResetPasswordResult reset_password(std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, const std::string& username,
                                   const std::string& password, std::uint32_t iterations = types::Password::default_iterations);

// The roles a new administrator holds when none were given: enough to administer users.
std::vector<std::string> default_roles();

}  // namespace athenasip::cli
