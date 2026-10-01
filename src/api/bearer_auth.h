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

#include "../types/user.h"
#include "sessions.h"

namespace athenasip::api {

namespace http = boost::beast::http;

// Who is calling, and what they may do.
//
// One kind of credential reaches this node: a session token, held by a user who logged in.
// The roles are the user's, read on every request so one taken away takes effect at once.
// Configured tokens were removed on 2026-10-01; the first administrator is made with
// `athenasip --add-user` on the host, and everything after that is a user.
class BearerAuth {
 public:
  // The caller, resolved. A route decides on the roles; the user is here because there are
  // routes where who you are decides what you may do to yourself.
  struct Caller {
    enum class Kind {
      none,  // nothing presented, or nothing that resolved: 401
      user,  // a session token, and the user holding it
    };

    Kind kind = Kind::none;
    std::vector<std::string> roles;

    // Set for Kind::user only: the user, and when the session it presented runs out.
    std::shared_ptr<types::User> user;
    std::time_t expires_at = 0;

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

      return "nobody";
    }
  };

  BearerAuth() = default;

  // Where session tokens are resolved. A node whose datastore cannot hold users has no way
  // in to its API at all, which every request is told with a 401.
  void sessions_register(std::shared_ptr<Sessions> sessions) { _sessions = std::move(sessions); }

  // The caller behind a presented token. Answers on the executor Sessions was given, or
  // inline when no store had to be asked.
  void resolve(const std::string& presented, std::function<void(Caller)> handler) const {
    if (presented.empty()) return handler(Caller{});

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
  std::shared_ptr<Sessions> _sessions;
};

}  // namespace athenasip::api
