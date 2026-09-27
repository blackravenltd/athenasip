//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "auth_api.h"

#include <boost/json.hpp>

#include "../loggers/logger_scoped.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

boost::json::array roles_json(const std::vector<std::string>& roles) {
  boost::json::array out;
  for (const auto& role : roles) out.push_back(boost::json::string(role));

  return out;
}

}  // namespace

AuthAPI::AuthAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Sessions> sessions, std::shared_ptr<BearerAuth> auth)
    : _logger(std::make_shared<loggers::LoggerScoped>("auth", std::move(logger))), _sessions(std::move(sessions)), _auth(std::move(auth)) {}

void AuthAPI::register_routes(Router& router) {
  auto self = shared_from_this();

  router.add(http::verb::post, "/api/v1/auth/login", Router::public_scope, [self](RouteContext c) { self->_login(std::move(c)); });
  router.add(http::verb::post, "/api/v1/auth/logout", Router::public_scope, [self](RouteContext c) { self->_logout(std::move(c)); });
  router.add(http::verb::get, "/api/v1/session", Router::public_scope, [self](RouteContext c) { self->_session(std::move(c)); });
}

void AuthAPI::_refuse(RouteContext& context) {
  // One body for all of them. Which of "no such user", "wrong password" and "that account
  // is disabled" it was is the thing not to say, because between them the answers are a
  // list of who holds an account on this node.
  context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
  write_error(context.response, http::status::unauthorized, "unauthorized", "the username or password is not right");
}

void AuthAPI::_unavailable(RouteContext& context, const std::string& reason) {
  _logger->error("could not check a credential: " + reason);
  write_error(context.response, http::status::service_unavailable, "unavailable", "the server cannot check credentials at the moment");
}

void AuthAPI::_login(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto username = string_field(*body, "username");
  const auto password = string_field(*body, "password");

  // A missing field is the caller's mistake and is worth saying so, because it is not a
  // guess at somebody's password. An empty one is a refusal rather than a 400, so that
  // probing with an empty password learns nothing a wrong password would not.
  if (!username || !password) {
    write_error(context.response, http::status::bad_request, "invalid_request", "username and password are required");
    return context.done();
  }

  auto self = shared_from_this();

  _sessions->login(*username, *password, [self, context](Sessions::Login answer) mutable {
    switch (answer.outcome) {
      case Sessions::Outcome::refused:
        self->_logger->warn("a login was refused: " + answer.reason);
        _refuse(context);
        return context.done();

      case Sessions::Outcome::unavailable:
        self->_unavailable(context, answer.reason);
        return context.done();

      case Sessions::Outcome::ok:
        break;
    }

    boost::json::object out;
    out["token"] = answer.value.token;

    // Unix seconds, agreed with the console. Every other time in this API is ISO 8601,
    // and this one is not, because a client computing how long it has left should not have
    // to parse a date to do it.
    out["expires_at"] = answer.value.expires_at;
    out["roles"] = roles_json(answer.value.roles);

    write_json(context.response, http::status::ok, out);
    context.done();
  });
}

void AuthAPI::_logout(RouteContext context) {
  // Nothing presented is not a logout. A client with no token has nothing to end and is
  // told so rather than being congratulated on it.
  if (context.bearer.empty()) {
    context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
    write_error(context.response, http::status::unauthorized, "unauthorized", "a bearer token is required");
    return context.done();
  }

  auto self = shared_from_this();

  _sessions->logout(context.bearer, [self, context](plugins::Status status) mutable {
    if (!status.ok) {
      self->_unavailable(context, status.error);
      return context.done();
    }

    // 204, as every other thing in this API that ends something answers. A token that
    // named no session gets it too: the state the caller asked for is that the session is
    // gone, and it is - which is also what stops a logout saying whether a token was real.
    write_no_content(context.response);
    context.done();
  });
}

void AuthAPI::_session(RouteContext context) {
  if (context.bearer.empty()) {
    context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
    write_error(context.response, http::status::unauthorized, "unauthorized", "a bearer token is required");
    return context.done();
  }

  // A configuration token first, and without asking the datastore: it is the credential
  // that has to keep working when the store is down, because it is how somebody gets back
  // in. A session token cannot collide with one - it is 32 random bytes - so the order
  // costs nothing.
  const auto scopes = _auth ? _auth->scopes_for(context.bearer) : std::vector<std::string>{};
  if (!scopes.empty()) {
    boost::json::object out;
    out["kind"] = "token";
    out["scopes"] = roles_json(scopes);
    out["roles"] = roles_json(BearerAuth::roles_for(scopes));

    // No expiry, deliberately: a configuration token lasts as long as it is in the file,
    // which is why it lives in one only root can read.
    write_json(context.response, http::status::ok, out);
    return context.done();
  }

  auto self = shared_from_this();

  _sessions->resolve(context.bearer, [self, context](Sessions::Lookup answer) mutable {
    switch (answer.outcome) {
      case Sessions::Outcome::refused:
        // The same 401 a login gets. An expired session and a token that was never real
        // are one answer for the same reason.
        _refuse(context);
        return context.done();

      case Sessions::Outcome::unavailable:
        self->_unavailable(context, answer.reason);
        return context.done();

      case Sessions::Outcome::ok:
        break;
    }

    const auto& user = *answer.value.user;

    boost::json::object out;
    out["kind"] = "user";
    out["username"] = user.username;
    out["display_name"] = user.display_name;

    // Read off the user on this request rather than off the session, so a role taken away
    // is gone here too. An empty list is an answer: a user with no roles may log in and is
    // told it holds none.
    out["roles"] = roles_json(user.roles);
    out["expires_at"] = answer.value.expires_at;

    write_json(context.response, http::status::ok, out);
    context.done();
  });
}

}  // namespace athenasip::api
