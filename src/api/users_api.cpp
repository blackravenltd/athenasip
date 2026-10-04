//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "users_api.h"

#include <ctime>
#include <utility>
#include <vector>

#include "../loggers/logger_scoped.h"
#include "../types/password.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

boost::json::array strings_json(const std::vector<std::string>& values) {
  boost::json::array out;
  for (const auto& value : values) out.push_back(boost::json::string(value));

  return out;
}

std::vector<std::string> strings_of(const boost::json::array& values) {
  std::vector<std::string> out;
  for (const auto& value : values) out.push_back(std::string(value.as_string()));

  return out;
}

// nullopt when absent: on a PUT, omitting "roles" leaves them alone and [] removes them all.
std::optional<boost::json::array> array_field(const boost::json::object& object, const std::string& name) {
  const auto it = object.find(name);
  if (it == object.end() || !it->value().is_array()) return std::nullopt;

  return it->value().as_array();
}

std::optional<bool> bool_field(const boost::json::object& object, const std::string& name) {
  const auto it = object.find(name);
  if (it == object.end() || !it->value().is_bool()) return std::nullopt;

  return it->value().as_bool();
}

}  // namespace

UsersAPI::UsersAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor,
                   std::shared_ptr<Sessions> sessions, std::uint32_t password_iterations)
    : _logger(std::make_shared<loggers::LoggerScoped>("users", std::move(logger))),
      _datastore(std::move(datastore)),
      _executor(std::move(executor)),
      _sessions(std::move(sessions)),
      _password_iterations(password_iterations) {}

void UsersAPI::register_routes(Router& router) {
  auto self = shared_from_this();

  const std::vector<std::string> manage = {types::roles::manage_admin_users};

  router.add(http::verb::get, "/api/v1/users", manage, [self](RouteContext c) { self->_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/users", manage, [self](RouteContext c) { self->_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/users/{user}", manage, [self](RouteContext c) { self->_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/users/{user}", manage, [self](RouteContext c) { self->_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/users/{user}", manage, [self](RouteContext c) { self->_delete(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/users/{user}/sessions", manage, [self](RouteContext c) { self->_revoke_sessions(std::move(c)); });

  // Any authenticated caller: the handler admits a user changing their own password with
  // the old one, and a holder of manage-admin-users changing anyone's without it.
  router.add(http::verb::post, "/api/v1/users/{user}/password", {}, [self](RouteContext c) { self->_set_password(std::move(c)); });
}

boost::json::object UsersAPI::_user_json(const types::User& user) {
  boost::json::object object;

  // The username as given, not the case-folded key.
  object["username"] = user.username;
  object["display_name"] = user.display_name;
  object["roles"] = strings_json(user.roles);
  object["disabled"] = user.disabled;

  // Unix seconds, as every time in this API is.
  object["created_at"] = static_cast<std::int64_t>(user.created_at);
  object["last_login_at"] = static_cast<std::int64_t>(user.last_login_at);

  // password_hash is never returned.
  return object;
}

bool UsersAPI::_is_self(const RouteContext& context, const std::string& username) {
  if (context.caller.kind != BearerAuth::Caller::Kind::user || !context.caller.user) return false;

  return context.caller.user->key() == types::User::normalise(username);
}

bool UsersAPI::_roles_are_known(const boost::json::array& roles, std::string& unknown) {
  for (const auto& role : roles) {
    if (!role.is_string()) {
      unknown = "a role that is not a string";
      return false;
    }

    const std::string name(role.as_string());
    if (!types::roles::is_known(name)) {
      unknown = name;
      return false;
    }
  }

  return true;
}

void UsersAPI::_with_user(const std::string& username, RouteContext context, std::function<void(std::shared_ptr<types::User>, RouteContext)> then) {
  _datastore->user_get(_executor, types::User::normalise(username), [context, then, username](plugins::Result<std::shared_ptr<types::User>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such user: " + username);
      return context.done();
    }

    then(result.value, std::move(context));
  });
}

void UsersAPI::_list(RouteContext context) {
  _datastore->user_list(_executor, [context](plugins::Result<std::vector<std::shared_ptr<types::User>>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    boost::json::array users;
    for (const auto& user : result.value) users.push_back(_user_json(*user));

    write_json(context.response, http::status::ok, users);
    context.done();
  });
}

void UsersAPI::_create(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto username = string_field(*body, "username");
  const auto password = string_field(*body, "password");

  if (!username || username->empty() || !password) {
    write_error(context.response, http::status::bad_request, "invalid_request", "username and password are required");
    return context.done();
  }

  auto user = std::make_shared<types::User>();
  user->username = *username;
  user->display_name = string_field(*body, "display_name").value_or(std::string());
  user->created_at = std::time(nullptr);

  if (const auto roles = array_field(*body, "roles")) {
    std::string unknown;
    if (!_roles_are_known(*roles, unknown)) {
      write_error(context.response, http::status::bad_request, "unknown_role", "there is no role called " + unknown);
      return context.done();
    }

    user->roles = strings_of(*roles);
  }

  user->password_hash = types::Password::hash(*password, _password_iterations);
  if (user->password_hash.empty()) {
    write_error(context.response, http::status::bad_request, "invalid_request", "the password cannot be empty");
    return context.done();
  }

  auto self = shared_from_this();

  _datastore->user_create(_executor, user, [self, context, user](plugins::Status status) mutable {
    if (!status.ok) {
      // user_create refuses an existing username, so a failure here is a conflict.
      self->_logger->debug("user_create refused: " + status.error);
      write_error(context.response, http::status::conflict, "conflict", "a user called " + user->username + " already exists");
      return context.done();
    }

    self->_logger->info("created user " + user->key());

    write_json(context.response, http::status::created, _user_json(*user));
    context.done();
  });
}

void UsersAPI::_get(RouteContext context) {
  const auto username = context.parameter("user");

  _with_user(username, std::move(context), [](std::shared_ptr<types::User> user, RouteContext context) {
    write_json(context.response, http::status::ok, _user_json(*user));
    context.done();
  });
}

void UsersAPI::_update(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto username = context.parameter("user");
  const auto self_edit = _is_self(context, username);
  auto self = shared_from_this();

  _with_user(username, std::move(context), [self, body, self_edit](std::shared_ptr<types::User> user, RouteContext context) {
    // user is a copy; it is changed and written back.
    if (const auto display_name = string_field(*body, "display_name")) user->display_name = *display_name;

    if (const auto roles = array_field(*body, "roles")) {
      std::string unknown;
      if (!_roles_are_known(*roles, unknown)) {
        write_error(context.response, http::status::bad_request, "unknown_role", "there is no role called " + unknown);
        return context.done();
      }

      auto wanted = strings_of(*roles);

      // A user cannot remove its own manage-admin-users: getting back in would need
      // `athenasip --add-user` on the host.
      if (self_edit && user->has_role(types::roles::manage_admin_users) &&
          std::find(wanted.begin(), wanted.end(), types::roles::manage_admin_users) == wanted.end()) {
        write_error(context.response, http::status::conflict, "would_lock_out", "a user cannot take manage-admin-users away from itself");
        return context.done();
      }

      user->roles = std::move(wanted);
    }

    if (const auto disabled = bool_field(*body, "disabled")) {
      if (self_edit && *disabled) {
        write_error(context.response, http::status::conflict, "would_lock_out", "a user cannot disable itself");
        return context.done();
      }

      user->disabled = *disabled;
    }

    self->_datastore->user_update(self->_executor, user, [self, context, user](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      // Revoke the sessions, so that disabling takes effect at once.
      if (user->disabled) self->_datastore->session_delete_for_user(self->_executor, user->key(), [](plugins::Status) {});

      write_json(context.response, http::status::ok, _user_json(*user));
      context.done();
    });
  });
}

void UsersAPI::_delete(RouteContext context) {
  const auto username = context.parameter("user");

  // A user cannot delete itself, for the same reason it cannot disable itself.
  if (_is_self(context, username)) {
    write_error(context.response, http::status::conflict, "would_lock_out", "a user cannot delete itself");
    return context.done();
  }

  auto self = shared_from_this();

  // The store answers a delete the same whether or not the user existed, so existence is
  // checked here.
  _with_user(username, std::move(context), [self](std::shared_ptr<types::User> user, RouteContext context) {
    self->_datastore->user_delete(self->_executor, user->key(), [self, context, user](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      self->_logger->info("deleted user " + user->key());

      write_no_content(context.response);
      context.done();
    });
  });
}

void UsersAPI::_revoke_sessions(RouteContext context) {
  const auto username = context.parameter("user");
  auto self = shared_from_this();

  // 404 for a user that does not exist, 204 for one that holds no sessions. The store does
  // not tell those apart, so existence is checked here.
  _with_user(username, std::move(context), [self](std::shared_ptr<types::User> user, RouteContext context) {
    self->_datastore->session_delete_for_user(self->_executor, user->key(), [self, context, user](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      self->_logger->info("revoked every session held by " + user->key());

      write_no_content(context.response);
      context.done();
    });
  });
}

void UsersAPI::_set_password(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto username = context.parameter("user");
  const auto password = string_field(*body, "password");

  if (!password || password->empty()) {
    write_error(context.response, http::status::bad_request, "invalid_request", "password is required");
    return context.done();
  }

  const auto manages = context.caller.has_role(types::roles::manage_admin_users);
  const auto self_edit = _is_self(context, username);

  // 403 before the store is asked, so this cannot be used to find out who exists.
  if (!manages && !self_edit) {
    write_error(context.response, http::status::forbidden, "forbidden", "this credential does not hold " + std::string(types::roles::manage_admin_users));
    return context.done();
  }

  const auto old_password = string_field(*body, "old_password");
  auto self = shared_from_this();

  _with_user(username, std::move(context), [self, password, old_password, manages, self_edit](std::shared_ptr<types::User> user, RouteContext context) {
    // Changing your own password needs the old one; holding the role does not, since it
    // exists to reset a lost one.
    if (self_edit && !manages) {
      if (!old_password || !types::Password::verify(*old_password, user->password_hash)) {
        // 403, not 401: the bearer is good and a client must not sign the user out. A code
        // of its own distinguishes it from the 403 for a missing role.
        write_error(context.response, http::status::forbidden, "wrong_password", "the old password is not right");
        return context.done();
      }
    }

    user->password_hash = types::Password::hash(*password, self->_password_iterations);
    if (user->password_hash.empty()) {
      write_error(context.response, http::status::internal_server_error, "internal_error", "the password could not be hashed");
      return context.done();
    }

    self->_datastore->user_update(self->_executor, user, [self, context, user](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      // Revoke every session the user held, including the caller's: a session issued under
      // the old password is as untrusted as the password.
      self->_datastore->session_delete_for_user(self->_executor, user->key(), [self, context](plugins::Status status) mutable {
        if (!status.ok) self->_logger->warn("could not revoke sessions after a password change: " + status.error);

        write_no_content(context.response);
        context.done();
      });
    });
  });
}

}  // namespace athenasip::api
