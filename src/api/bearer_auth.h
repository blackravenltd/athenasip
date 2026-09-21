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
    const auto presented = _presented_token(request);
    if (presented.empty()) return Result::missing;

    for (const auto& token : _tokens) {
      // Length first, then every byte: a comparison that stops at the first difference
      // tells the caller how much of a guess was right.
      if (!_equal(token.token, presented)) continue;

      return token.has_scope(scope) ? Result::ok : Result::forbidden;
    }

    return Result::unknown;
  }

 private:
  static std::string _presented_token(const http::request<http::string_body>& request) {
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

  static bool _equal(const std::string& expected, const std::string& presented) {
    if (expected.size() != presented.size()) return false;

    unsigned char difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(expected[i] ^ presented[i]);

    return difference == 0;
  }

  std::vector<Config::ApiToken> _tokens;
};

}  // namespace athenasip::api
