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

  // Blocking is right here: a command on its own thread, with no strand to block.
  std::promise<plugins::Status> promise;
  auto future = promise.get_future();

  datastore->user_create(executor, user, [&promise](plugins::Status status) { promise.set_value(std::move(status)); });

  const auto status = future.get();

  if (!status.ok) {
    // Ask whether the user exists rather than parsing the error: drivers word it differently.
    std::promise<plugins::Result<std::shared_ptr<types::User>>> existing;
    auto found = existing.get_future();

    datastore->user_get(executor, user->key(), [&existing](plugins::Result<std::shared_ptr<types::User>> result) { existing.set_value(std::move(result)); });

    const auto answer = found.get();
    if (answer.ok && answer.value) return fail(Result::Outcome::taken, "a user called " + user->key() + " already exists");

    return fail(Result::Outcome::refused, status.error);
  }

  return Result{Result::Outcome::created, user->key()};
}

ResetPasswordResult reset_password(std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, const std::string& username,
                                   const std::string& password, std::uint32_t iterations) {
  using Reset = ResetPasswordResult;

  if (!datastore) return Reset{Reset::Outcome::invalid, "there is no datastore to write to"};
  if (username.empty()) return Reset{Reset::Outcome::invalid, "a username is required"};
  if (password.empty()) return Reset{Reset::Outcome::invalid, "a password is required"};

  const auto key = types::User::normalise(username);

  std::promise<plugins::Result<std::shared_ptr<types::User>>> read;
  auto reading = read.get_future();
  datastore->user_get(executor, key, [&read](plugins::Result<std::shared_ptr<types::User>> result) { read.set_value(std::move(result)); });

  const auto found = reading.get();
  if (!found.ok) return Reset{Reset::Outcome::refused, found.error};
  if (!found.value) return Reset{Reset::Outcome::missing, "there is no user called " + key};

  auto user = found.value;
  user->password_hash = types::Password::hash(password, iterations);
  if (user->password_hash.empty()) return Reset{Reset::Outcome::invalid, "the password could not be hashed"};

  std::promise<plugins::Status> written;
  auto writing = written.get_future();
  datastore->user_update(executor, user, [&written](plugins::Status status) { written.set_value(std::move(status)); });

  const auto updated = writing.get();
  if (!updated.ok) return Reset{Reset::Outcome::refused, updated.error};

  std::promise<plugins::Status> ended;
  auto ending = ended.get_future();
  datastore->session_delete_for_user(executor, key, [&ended](plugins::Status status) { ended.set_value(std::move(status)); });

  // The password is changed either way; a session that could not be ended is reported.
  const auto signed_out = ending.get();
  if (!signed_out.ok) return Reset{Reset::Outcome::refused, "the password is changed, but its sessions could not be ended: " + signed_out.error};

  return Reset{Reset::Outcome::reset, key};
}

}  // namespace athenasip::cli
