//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_add_user.h"

#include <ctime>
#include <future>

#include "types/password.h"
#include "types/user.h"

namespace athenasip::cli {

namespace {

using Result = AddUserResult;

Result fail(Result::Outcome outcome, std::string message) { return Result{outcome, std::move(message)}; }

}  // namespace

std::vector<std::string> default_roles() { return {types::roles::manage_admin_users}; }

Result add_user(std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, const std::string& username, const std::string& display_name,
                const std::vector<std::string>& roles, const std::string& password, std::uint32_t iterations) {
  if (!datastore) return fail(Result::Outcome::invalid, "there is no datastore to write to");
  if (username.empty()) return fail(Result::Outcome::invalid, "a username is required");
  if (password.empty()) return fail(Result::Outcome::invalid, "a password is required");

  for (const auto& role : roles) {
    if (!types::roles::is_known(role)) return fail(Result::Outcome::unknown_role, "there is no role called " + role);
  }

  auto user = std::make_shared<types::User>();
  user->username = username;
  user->display_name = display_name;
  user->roles = roles.empty() ? default_roles() : roles;
  user->created_at = std::time(nullptr);
  user->password_hash = types::Password::hash(password, iterations);

  if (user->password_hash.empty()) return fail(Result::Outcome::invalid, "the password could not be hashed");

  // The one place in this tree where waiting on the contract is right: this is a command
  // with nothing to serve, on its own thread, and there is no strand to block.
  std::promise<plugins::Status> promise;
  auto future = promise.get_future();

  datastore->user_create(executor, user, [&promise](plugins::Status status) { promise.set_value(std::move(status)); });

  const auto status = future.get();

  if (!status.ok) {
    // Why it refused, asked rather than read out of the message. The contract makes create
    // refuse an existing username, but it does not say what the driver calls that: Redis
    // answers "user_create: tom already exists" and the memory driver answers "user_create
    // failed", and an operator told the wrong one of those goes looking in the wrong
    // place. The extra round trip is on the failure path only.
    std::promise<plugins::Result<std::shared_ptr<types::User>>> existing;
    auto found = existing.get_future();

    datastore->user_get(executor, user->key(), [&existing](plugins::Result<std::shared_ptr<types::User>> result) { existing.set_value(std::move(result)); });

    const auto answer = found.get();
    if (answer.ok && answer.value) return fail(Result::Outcome::taken, "a user called " + user->key() + " already exists");

    return fail(Result::Outcome::refused, status.error);
  }

  return Result{Result::Outcome::created, user->key()};
}

}  // namespace athenasip::cli
