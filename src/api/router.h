//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "admin_api.h"
#include "api_json.h"
#include "bearer_auth.h"

namespace athenasip::api {

namespace http = boost::beast::http;

// What a handler is given. The body and the parameters are copies rather than views
// into the request, because a handler is async: it goes to the datastore and answers
// when the store does, by which time the request it came from may be several frames
// down the stack.
struct RouteContext {
  std::unordered_map<std::string, std::string> parameters;
  std::unordered_map<std::string, std::string> query;
  std::string body;

  // The bearer token this request presented, empty when it presented none. The router has
  // already decided whether it admits the request; this is for the routes that need the
  // token itself rather than a verdict on it, which is the logout ending the session it
  // was given.
  std::string bearer;

  // Who is calling, resolved by the router before the handler ran. Meaningful on a route
  // that required a credential; a `Kind::none` caller on an open route means only that
  // nobody asked.
  BearerAuth::Caller caller;

  std::shared_ptr<http::response<http::string_body>> response;

  // Sends what the handler has put in the response. Exactly once, on whatever thread
  // the handler finished on.
  std::function<void()> done;

  // By value, deliberately. A reference into this map outlives nothing: the usual
  // shape here is to read a parameter and move the context into a handler in the same
  // call, and a reference would then point into a map that has already been moved from.
  std::string parameter(const std::string& name) const {
    const auto it = parameters.find(name);
    return it == parameters.end() ? std::string() : it->second;
  }
};

// Method, path and the roles that admit it, in one table. A route that forgets to say who
// may call it does not compile, which is the point: authorisation is part of declaring a
// route rather than something a handler remembers to do.
class Router {
 public:
  using Handler = std::function<void(RouteContext)>;

  explicit Router(std::shared_ptr<BearerAuth> auth) : _auth(std::move(auth)) {}

  // Any of `roles` admits. An empty set means any authenticated caller, which is the safe
  // thing for it to mean: a route declared without naming roles demands a credential and
  // grants nothing, so forgetting to name them cannot open a route to the world.
  void add(http::verb method, std::string pattern, std::vector<std::string> roles, Handler handler) {
    _routes.push_back(Route{method, _split(pattern), std::move(roles), false, std::move(handler)});
  }

  // No credential at all, and it has to be asked for by name. Only for what a load
  // balancer or a container healthcheck reaches before it has one, and for the route that
  // hands credentials out.
  void add_open(http::verb method, std::string pattern, Handler handler) { _routes.push_back(Route{method, _split(pattern), {}, true, std::move(handler)}); }

  // The middleware for the chain. It answers anything under its own prefix and passes
  // everything else along, so static files and the API can share a port.
  HttpMiddleware middleware(std::string prefix) {
    return [this, prefix](const http::request<http::string_body>& request, std::shared_ptr<http::response<http::string_body>> response,
                          std::function<void(bool)> next) { _handle(prefix, request, std::move(response), std::move(next)); };
  }

 private:
  struct Route {
    http::verb method;
    std::vector<std::string> pattern;
    std::vector<std::string> roles;
    bool open = false;
    Handler handler;
  };

  void _handle(const std::string& prefix, const http::request<http::string_body>& request, std::shared_ptr<http::response<http::string_body>> response,
               std::function<void(bool)> next) {
    const std::string target(request.target());
    if (target.rfind(prefix, 0) != 0) return next(true);

    const auto question = target.find('?');
    const auto path = question == std::string::npos ? target : target.substr(0, question);
    const auto segments = _split(path);

    // A path that matches no route at all is a 404; one that matches a route with the
    // wrong method is a 405, and saying so saves a client guessing at the spelling.
    bool path_matched = false;

    for (const auto& route : _routes) {
      std::unordered_map<std::string, std::string> parameters;
      if (!_matches(route.pattern, segments, parameters)) continue;

      path_matched = true;
      if (route.method != request.method()) continue;

      // Everything out of the request and into the context first. Resolving a session
      // token is a datastore round trip, so by the time the credential is known the
      // request object may be several frames down the stack.
      RouteContext context;
      context.parameters = std::move(parameters);
      context.query = _parse_query(question == std::string::npos ? std::string() : target.substr(question + 1));
      context.body = request.body();
      context.bearer = BearerAuth::presented_token(request);
      context.response = response;
      context.done = [next]() { next(false); };

      if (route.open) return route.handler(std::move(context));

      if (!_auth) {
        write_error(response, http::status::unauthorized, "unauthorized", "no credentials are configured, so nothing may be called");
        return next(false);
      }

      const auto presented = context.bearer;

      _auth->resolve(presented, [context, roles = route.roles, handler = route.handler](BearerAuth::Caller caller) mutable {
        if (caller.unavailable) {
          write_error(context.response, http::status::service_unavailable, "unavailable", "the server cannot check credentials at the moment");
          return context.done();
        }

        if (!caller.authenticated()) {
          context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
          write_error(context.response, http::status::unauthorized, "unauthorized", "a bearer token is required");
          return context.done();
        }

        // An empty set is any authenticated caller; otherwise one of the named roles has
        // to be held. A real credential that holds none is a 403, not a 401: it is who it
        // says it is and may not do this.
        if (!roles.empty() && !caller.has_any(roles)) {
          write_error(context.response, http::status::forbidden, "forbidden", "this credential does not hold " + _describe(roles));
          return context.done();
        }

        context.caller = std::move(caller);
        handler(std::move(context));
      });

      return;
    }

    if (path_matched) {
      write_error(response, http::status::method_not_allowed, "method_not_allowed", "that path does not take this method");
      return next(false);
    }

    write_error(response, http::status::not_found, "not_found", "no such endpoint");
    next(false);
  }

  // What a 403 says it was missing. The role names are the API's own vocabulary and are
  // safe to name: knowing that a route wants manage-realms tells a caller nothing it could
  // not read in the documentation.
  static std::string _describe(const std::vector<std::string>& roles) {
    if (roles.size() == 1) return roles.front();

    std::string out = "any of ";
    for (std::size_t i = 0; i < roles.size(); ++i) out += (i ? ", " : "") + roles[i];

    return out;
  }

  static std::vector<std::string> _split(const std::string& path) {
    std::vector<std::string> segments;

    std::size_t start = 0;
    while (start <= path.size()) {
      const auto slash = path.find('/', start);
      auto segment = slash == std::string::npos ? path.substr(start) : path.substr(start, slash - start);

      if (!segment.empty()) segments.push_back(std::move(segment));
      if (slash == std::string::npos) break;

      start = slash + 1;
    }

    return segments;
  }

  static bool _matches(const std::vector<std::string>& pattern, const std::vector<std::string>& segments, std::unordered_map<std::string, std::string>& out) {
    if (pattern.size() != segments.size()) return false;

    for (std::size_t i = 0; i < pattern.size(); ++i) {
      const auto& expected = pattern[i];

      if (expected.size() > 2 && expected.front() == '{' && expected.back() == '}') {
        out[expected.substr(1, expected.size() - 2)] = percent_decode(segments[i]);
        continue;
      }

      if (expected != segments[i]) return false;
    }

    return true;
  }

  static std::unordered_map<std::string, std::string> _parse_query(const std::string& query) {
    std::unordered_map<std::string, std::string> parsed;

    std::size_t start = 0;
    while (start < query.size()) {
      auto amp = query.find('&', start);
      if (amp == std::string::npos) amp = query.size();

      const auto part = query.substr(start, amp - start);
      const auto equals = part.find('=');

      if (equals == std::string::npos) {
        if (!part.empty()) parsed[percent_decode(part)] = std::string();
      } else {
        parsed[percent_decode(part.substr(0, equals))] = percent_decode(part.substr(equals + 1));
      }

      start = amp + 1;
    }

    return parsed;
  }

 public:
  // A SIP user is a URI component and arrives percent-encoded: alice%40example.com in a
  // path is one user called alice@example.com, not two segments.
  static std::string percent_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '+') {
        out.push_back(' ');
        continue;
      }

      if (value[i] != '%' || i + 2 >= value.size() || !std::isxdigit(static_cast<unsigned char>(value[i + 1])) ||
          !std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
        out.push_back(value[i]);
        continue;
      }

      out.push_back(static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    }

    return out;
  }

 private:
  std::shared_ptr<BearerAuth> _auth;
  std::vector<Route> _routes;
};

}  // namespace athenasip::api
