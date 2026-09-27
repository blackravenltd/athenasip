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

// An array field, absent rather than empty when it is not there: on a PUT, "roles not
// given" leaves them alone and "roles given as []" takes them all away.
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

  // Any authenticated caller, because "this is my own account" is not something a role can
  // express. The handler admits somebody changing their own password with the old one, and
  // anybody holding manage-admin-users changing anyone's without it.
  router.add(http::verb::post, "/api/v1/users/{user}/password", {}, [self](RouteContext c) { self->_set_password(std::move(c)); });
}

boost::json::object UsersAPI::_user_json(const types::User& user) {
  boost::json::object object;

  // The username as it was given, not the key it is filed under, which is case-folded.
  object["username"] = user.username;
  object["display_name"] = user.display_name;
  object["roles"] = strings_json(user.roles);
  object["disabled"] = user.disabled;

  // Unix seconds, as every other time this API hands back.
  object["created_at"] = static_cast<std::int64_t>(user.created_at);
  object["last_login_at"] = static_cast<std::int64_t>(user.last_login_at);

  // password_hash is deliberately absent. It is the password in the only form this node
  // holds it, and an API that hands it back is one that puts it in every log that records
  // a response.
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
      // The contract makes create refuse an existing username, which is what lets this be
      // a conflict rather than an overwrite.
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
    // A read hands back a copy, so this is the copy being changed and written back.
    if (const auto display_name = string_field(*body, "display_name")) user->display_name = *display_name;

    if (const auto roles = array_field(*body, "roles")) {
      std::string unknown;
      if (!_roles_are_known(*roles, unknown)) {
        write_error(context.response, http::status::bad_request, "unknown_role", "there is no role called " + unknown);
        return context.done();
      }

      auto wanted = strings_of(*roles);

      // Nobody locks themselves out of the door they are standing in. Agreed with the
      // console, and the reason is not politeness: an administrator who does this to
      // themselves by accident needs the configuration token to get back in, which is a
      // file on a host they may not have.
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

      // Disabling is immediate rather than eventual, which is what the sessions being
      // revocable is for. Without this the user keeps working until each token expires.
      if (user->disabled) self->_datastore->session_delete_for_user(self->_executor, user->key(), [](plugins::Status) {});

      write_json(context.response, http::status::ok, _user_json(*user));
      context.done();
    });
  });
}

void UsersAPI::_delete(RouteContext context) {
  const auto username = context.parameter("user");

  // The same rule as disabling, for the same reason: a user that deleted itself has locked
  // itself out just as thoroughly, and leaving the hole open would make the other check
  // decorative.
  if (_is_self(context, username)) {
    write_error(context.response, http::status::conflict, "would_lock_out", "a user cannot delete itself");
    return context.done();
  }

  auto self = shared_from_this();

  // The existence check is the handler's, because the store answers a delete the same way
  // whether or not anything was there.
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

  // 404 for a user that does not exist; 204 for one that exists and holds nothing, because
  // holding nothing is the state the caller asked for. The store will not tell those apart,
  // so the existence check happens here.
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

  // Somebody else's password, without the role, is not something to answer in detail: 403
  // before the store is asked, so this cannot be used to find out who exists.
  if (!manages && !self_edit) {
    write_error(context.response, http::status::forbidden, "forbidden", "this credential does not hold " + std::string(types::roles::manage_admin_users));
    return context.done();
  }

  const auto old_password = string_field(*body, "old_password");
  auto self = shared_from_this();

  _with_user(username, std::move(context), [self, password, old_password, manages, self_edit](std::shared_ptr<types::User> user, RouteContext context) {
    // Your own password needs the old one. Holding the role does not, because the case it
    // exists for is somebody who has lost theirs.
    if (self_edit && !manages) {
      if (!old_password || !types::Password::verify(*old_password, user->password_hash)) {
        context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
        write_error(context.response, http::status::unauthorized, "unauthorized", "the old password is not right");
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

      // Every session the user held, including the one that asked. A password is changed
      // because the old one is no longer trusted, and a session issued against it is
      // exactly as untrusted; an administrator resetting a compromised account would
      // otherwise leave whoever compromised it logged in.
      self->_datastore->session_delete_for_user(self->_executor, user->key(), [self, context](plugins::Status status) mutable {
        if (!status.ok) self->_logger->warn("could not revoke sessions after a password change: " + status.error);

        write_no_content(context.response);
        context.done();
      });
    });
  });
}

}  // namespace athenasip::api
