//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../config.h"
#include "../types/user.h"
#include "sessions.h"

namespace athenasip::api {

namespace http = boost::beast::http;

// Who is calling, and what they may do.
//
// Two kinds of credential reach this node and both end up as a set of roles, which is the
// only vocabulary the routes speak:
//
//   a session token, held by a user who logged in - the roles are the user's, read on
//   every request so one taken away takes effect immediately;
//   a configuration token, held by a machine or by whoever is getting back in - the roles
//   come from its scopes.
//
// Resolving the first means asking the datastore, so this is asynchronous. The second is
// answered without touching the store and is therefore tried first: it is the credential
// that has to keep working when the store is the thing that is broken.
class BearerAuth {
 public:
  // The caller, resolved. A route decides on the roles; the user is here because there are
  // routes where who you are decides what you may do to yourself.
  struct Caller {
    enum class Kind {
      none,   // nothing presented, or nothing that resolved: 401
      token,  // a configuration token
      user,   // a session token, and the user holding it
    };

    Kind kind = Kind::none;
    std::vector<std::string> roles;

    // Set for Kind::user only: the user, and when the session it presented runs out.
    std::shared_ptr<types::User> user;
    std::time_t expires_at = 0;

    // Set for Kind::token only. Kept beside the roles they map to because the scopes are
    // what an operator wrote in the file and so are what a report should name back.
    std::vector<std::string> scopes;

    // The store could not be asked, which is not a refusal: 503, not 401.
    bool unavailable = false;

    bool authenticated() const { return kind != Kind::none; }

    bool has_role(const std::string& role) const { return std::find(roles.begin(), roles.end(), role) != roles.end(); }

    // Any of them admits, which is what a route's role set means.
    bool has_any(const std::vector<std::string>& wanted) const {
      return std::any_of(wanted.begin(), wanted.end(), [this](const std::string& role) { return has_role(role); });
    }

    // For a log line, never for a response body.
    std::string describe() const {
      if (kind == Kind::user) return user ? user->key() : "a user";
      if (kind == Kind::token) return "a configuration token";

      return "nobody";
    }
  };

  explicit BearerAuth(std::vector<Config::ApiToken> tokens) : _tokens(std::move(tokens)) {}

  // Where session tokens are resolved. Optional: a node whose datastore cannot hold users
  // has configuration tokens and nothing else, which is a working node rather than a
  // broken one.
  void sessions_register(std::shared_ptr<Sessions> sessions) { _sessions = std::move(sessions); }

  // The caller behind a presented token. Answers on the executor Sessions was given, or
  // inline when no store had to be asked.
  void resolve(const std::string& presented, std::function<void(Caller)> handler) const {
    if (presented.empty()) return handler(Caller{});

    // A configuration token first, and without the datastore. A session token cannot
    // collide with one, being 32 random bytes, so the order costs nothing and buys the way
    // back in when the store is down.
    if (auto scopes = scopes_for(presented)) {
      Caller caller;
      caller.kind = Caller::Kind::token;
      caller.roles = roles_for(*scopes);
      caller.scopes = std::move(*scopes);

      return handler(std::move(caller));
    }

    if (!_sessions) return handler(Caller{});

    _sessions->resolve(presented, [handler](Sessions::Lookup answer) {
      Caller caller;

      if (answer.outcome == Sessions::Outcome::unavailable) {
        caller.unavailable = true;
        return handler(std::move(caller));
      }

      if (!answer.ok()) return handler(std::move(caller));

      caller.kind = Caller::Kind::user;
      caller.user = answer.value.user;
      caller.roles = answer.value.user->roles;
      caller.expires_at = answer.value.expires_at;

      handler(std::move(caller));
    });
  }

  // The scopes a presented configuration token holds. Absent for a token this node does
  // not know, which is a different answer from a token that is known and holds none: the
  // second is a real credential authorised for nothing, and is a 403 rather than a 401.
  std::optional<std::vector<std::string>> scopes_for(const std::string& presented) const {
    for (const auto& token : _tokens) {
      // Length first, then every byte: a comparison that stops at the first difference
      // tells the caller how much of a guess was right.
      if (_equal(token.token, presented)) return token.scopes;
    }

    return std::nullopt;
  }

  // What a configuration token may do, in the role vocabulary the routes use.
  //
  // admin is every role there is. It is the one credential on this node that can do
  // everything, it lives in a file only root can read, and the intent is that a
  // deployment stops using it once real users exist. client is exactly what a SIP client
  // application reaches today and has no business in the rest of the model.
  static std::vector<std::string> roles_for(const std::vector<std::string>& scopes) {
    std::vector<std::string> roles;

    for (const auto& scope : scopes) {
      if (scope == "admin") return types::roles::all();
      if (scope == "client") roles.push_back(types::roles::view_cluster_status);
    }

    return roles;
  }

  // How a bearer token is read off a request: one answer for the router, for this class,
  // and for a handler that needs the token itself rather than a verdict on it.
  static std::string presented_token(const http::request<http::string_body>& request) {
    const auto header = request[http::field::authorization];
    if (header.empty()) return {};

    const std::string value(header);
    const std::string prefix = "Bearer ";
    if (value.size() <= prefix.size()) return {};

    // RFC 7235 makes the scheme case-insensitive.
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return {};
    }

    std::size_t start = prefix.size();
    while (start < value.size() && value[start] == ' ') ++start;

    return value.substr(start);
  }

 private:
  static bool _equal(const std::string& expected, const std::string& presented) {
    if (expected.size() != presented.size()) return false;

    unsigned char difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(expected[i] ^ presented[i]);

    return difference == 0;
  }

  std::vector<Config::ApiToken> _tokens;
  std::shared_ptr<Sessions> _sessions;
};

}  // namespace athenasip::api
