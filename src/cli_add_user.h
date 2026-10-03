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

// Creating an administrator without the API.
//
// The way back in: a node whose admin passwords have all been lost, or whose HTTP
// listener is not reachable, still has a datastore and a person with shell access. It is
// also how the first administrator is made, there being no configured token to do it, and
// it is why losing every password is an inconvenience rather than a rebuild.
//
// It goes to the datastore and touches nothing else - no listeners, no event bus, no
// Core - so it is safe to run against a node that is already serving.
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

// Blocking, deliberately: this is a command-line tool on its own thread with nothing to
// serve, which is the one situation where waiting on the async contract is the right
// answer. Roles are validated here because the datastore carries one it does not
// recognise rather than dropping it.
AddUserResult add_user(std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, const std::string& username,
                       const std::string& display_name, const std::vector<std::string>& roles, const std::string& password,
                       std::uint32_t iterations = types::Password::default_iterations);

// A new password for a user that exists, the same way: from the host, to the datastore,
// with nothing else started. The other way back in, for a user who has lost a password
// rather than a node that has lost every user. Every session the user holds ends with it,
// because a password is reset when the old one is lost or known to somebody else, and
// whoever holds it may be signed in with it.
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

// What a new administrator holds when the command line named no roles. A recovery user
// that cannot administer anybody is not a way back in.
std::vector<std::string> default_roles();

}  // namespace athenasip::cli
