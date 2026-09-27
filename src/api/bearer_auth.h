//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/beast/http.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../config.h"
#include "../types/user.h"

namespace athenasip::api {

namespace http = boost::beast::http;

// Who is calling, and whether they may. Tokens come from the config for now; the shape
// is what a datastore-backed token table would replace.
//
// Two scopes: admin provisions, client reads what a client may see. A route names the
// scope it needs and nothing else decides, so adding a route cannot accidentally leave
// it open.
class BearerAuth {
 public:
  enum class Result {
    ok,
    missing,    // no Authorization header, or not a Bearer one: 401
    unknown,    // a token, but not one we know: 401
    forbidden,  // a token we know, without the scope this route needs: 403
  };

  explicit BearerAuth(std::vector<Config::ApiToken> tokens) : _tokens(std::move(tokens)) {}

  Result check(const http::request<http::string_body>& request, const std::string& scope) const {
    const auto presented = presented_token(request);
    if (presented.empty()) return Result::missing;

    for (const auto& token : _tokens) {
      // Length first, then every byte: a comparison that stops at the first difference
      // tells the caller how much of a guess was right.
      if (!_equal(token.token, presented)) continue;

      return token.has_scope(scope) ? Result::ok : Result::forbidden;
    }

    return Result::unknown;
  }

  // The scopes a presented token holds, empty for a token this node does not know. It is
  // how a route that has to tell one kind of credential from the other - `GET /session`
  // is the one - asks about a configuration token without the answer being yes or no.
  std::vector<std::string> scopes_for(const std::string& presented) const {
    for (const auto& token : _tokens) {
      if (_equal(token.token, presented)) return token.scopes;
    }

    return {};
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
};

}  // namespace athenasip::api
