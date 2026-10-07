//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "admin_api.h"
#include "api_json.h"
#include "bearer_auth.h"
#include "rate_limiter.h"
#include "response_stream.h"
#include "sessions.h"
#include "subscriber_auth.h"

namespace athenasip::api {

namespace http = boost::beast::http;

// What a handler is given. The body and parameters are copies, not views into the
// request: handlers are async and may answer after the request is gone.
struct RouteContext {
  std::unordered_map<std::string, std::string> parameters;
  std::unordered_map<std::string, std::string> query;
  std::string body;

  // The bearer token presented, empty when there was none. For routes that need the token
  // itself, as logout does.
  std::string bearer;

  // The caller, resolved by the router before the handler runs. On an open route it is
  // always Kind::none.
  BearerAuth::Caller caller;

  // The peer address, which open routes are limited by.
  std::string remote;

  // On a subscriber route, the subscriber the request authenticated as (SubscriberAuth).
  std::shared_ptr<types::Subscriber> subscriber;

  std::shared_ptr<http::response<http::string_body>> response;

  // Sends the response. Call exactly once, on any thread.
  std::function<void()> done;

  // Makes the response a stream that stays open after its headers (Server-Sent Events): `start` is given the
  // stream once the headers are out. Call before done().
  std::function<void(StreamStart)> stream;

  // Returns by value: callers read a parameter and move the context in the same call, and
  // a reference would dangle.
  std::string parameter(const std::string& name) const {
    const auto it = parameters.find(name);
    return it == parameters.end() ? std::string() : it->second;
  }
};

// Method, path and the roles that admit it, in one table: authorisation is part of
// declaring a route, not something a handler remembers to do.
class Router {
 public:
  using Handler = std::function<void(RouteContext)>;

  explicit Router(std::shared_ptr<BearerAuth> auth, std::shared_ptr<Throttle> throttle = std::make_shared<Throttle>())
      : _auth(std::move(auth)), _throttle(std::move(throttle)) {}

  // Every route is limited: open routes and unresolved credentials by source address,
  // signed-in callers by session. Routes that limit further, as the login does, use this.
  std::shared_ptr<Throttle> throttle() const { return _throttle; }

  // 429 with the wait in Retry-After (RFC 6585, RFC 9110 10.2.3) and in the message.
  static void write_too_many(const std::shared_ptr<http::response<http::string_body>>& response, std::chrono::seconds wait) {
    write_error(response, http::status::too_many_requests, "rate_limited",
                "too many requests; try again in " + std::to_string(wait.count()) + (wait.count() == 1 ? " second" : " seconds"));
    response->set(http::field::retry_after, std::to_string(wait.count()));
  }

  // Any of `roles` admits. An empty set means any authenticated caller, so a route that
  // names no roles still demands a credential.
  void add(http::verb method, std::string pattern, std::vector<std::string> roles, Handler handler) {
    _routes.push_back(Route{method, _split(pattern), std::move(roles), false, std::move(handler)});
  }

  // A route that needs no credential: healthchecks, and the login that hands credentials out.
  void add_open(http::verb method, std::string pattern, Handler handler) { _routes.push_back(Route{method, _split(pattern), {}, true, std::move(handler)}); }

  // A route for a subscriber, authenticated with its SIP credentials (SubscriberAuth). Every one lives under
  // /api/v1/subscriber/{realm}/, and a subscriber's credentials open nothing else.
  void add_subscriber(http::verb method, std::string pattern, Handler handler) {
    if (pattern.rfind(kSubscriberPrefix, 0) != 0) throw std::logic_error("a subscriber route must be under " + std::string(kSubscriberPrefix) + ": " + pattern);
    Route route{method, _split(pattern), {}, false, std::move(handler)};
    route.subscriber = true;
    _routes.push_back(std::move(route));
  }

  static constexpr char kSubscriberPrefix[] = "/api/v1/subscriber/{realm}/";

  void subscriber_auth_register(std::shared_ptr<SubscriberAuth> auth) { _subscriber_auth = std::move(auth); }

  // The middleware for the chain. It answers everything under its prefix and passes the
  // rest along, so static files and the API can share a port.
  HttpMiddleware middleware(std::string prefix) {
    return
        [this, prefix](const http::request<http::string_body>& request, const std::string& remote, std::shared_ptr<http::response<http::string_body>> response,
                       std::function<void(bool)> next) { _handle(prefix, request, remote, std::move(response), std::move(next)); };
  }

  // Where streaming routes leave their streams; the listener the router serves on takes them (AdminAPI).
  std::shared_ptr<StreamRegistry> streams() const { return _streams; }

 private:
  struct Route {
    http::verb method;
    std::vector<std::string> pattern;
    std::vector<std::string> roles;
    bool open = false;
    Handler handler;
    bool subscriber = false;
  };

  void _handle(const std::string& prefix, const http::request<http::string_body>& request, const std::string& remote,
               std::shared_ptr<http::response<http::string_body>> response, std::function<void(bool)> next) {
    const std::string target(request.target());
    if (target.rfind(prefix, 0) != 0) return next(true);

    const auto question = target.find('?');
    const auto path = question == std::string::npos ? target : target.substr(0, question);
    const auto segments = _split(path);

    // No route matches the path: 404. A route matches with another method: 405.
    bool path_matched = false;

    for (const auto& route : _routes) {
      std::unordered_map<std::string, std::string> parameters;
      if (!_matches(route.pattern, segments, parameters)) continue;

      path_matched = true;
      if (route.method != request.method()) continue;

      // Copy everything out of the request first: resolving the token is a datastore round
      // trip, and the request may be gone when it answers.
      RouteContext context;
      context.parameters = std::move(parameters);
      context.query = _parse_query(question == std::string::npos ? std::string() : target.substr(question + 1));
      context.body = request.body();
      context.bearer = BearerAuth::presented_token(request);
      context.remote = remote;
      context.response = response;
      context.done = [next]() { next(false); };
      context.stream = [streams = _streams, response](StreamStart start) { streams->add(response.get(), std::move(start)); };

      if (route.subscriber) return _handle_subscriber(route, request, target, std::move(context), std::move(next));

      if (route.open) {
        if (const auto wait = _throttle->take(_source_key(remote), _throttle->limits().open)) {
          write_too_many(response, *wait);
          return next(false);
        }

        return route.handler(std::move(context));
      }

      if (!_auth) {
        write_error(response, http::status::unauthorized, "unauthorized", "no credentials are configured, so nothing may be called");
        return next(false);
      }

      const auto presented = context.bearer;

      _auth->resolve(presented, [throttle = _throttle, context, roles = route.roles, handler = route.handler](BearerAuth::Caller caller) mutable {
        if (caller.unavailable) {
          write_error(context.response, http::status::service_unavailable, "unavailable", "the server cannot check credentials at the moment");
          return context.done();
        }

        // An unresolved credential is limited as an open route is, by source address.
        if (!caller.authenticated()) {
          if (const auto wait = throttle->take(_source_key(context.remote), throttle->limits().open)) {
            write_too_many(context.response, *wait);
            return context.done();
          }

          context.response->set(http::field::www_authenticate, "Bearer realm=\"athenasip\"");
          write_error(context.response, http::status::unauthorized, "unauthorized", "a bearer token is required");
          return context.done();
        }

        // Limited by session, keyed by the token's hash, before the role check.
        if (const auto wait = throttle->take("session:" + Sessions::token_hash(context.bearer), throttle->limits().session)) {
          write_too_many(context.response, *wait);
          return context.done();
        }

        // A real credential holding none of the named roles is a 403, not a 401.
        if (!roles.empty() && !caller.has_any(roles)) {
          write_error(context.response, http::status::forbidden, "forbidden", "this credential does not hold " + _describe(roles));
          return context.done();
        }

        context.caller = std::move(caller);
        handler(std::move(context));
      });

      return;
    }

    // Unknown endpoints are limited too, against scanning.
    if (const auto wait = _throttle->take(_source_key(remote), _throttle->limits().open)) {
      write_too_many(response, *wait);
      return next(false);
    }

    if (path_matched) {
      write_error(response, http::status::method_not_allowed, "method_not_allowed", "that path does not take this method");
      return next(false);
    }

    write_error(response, http::status::not_found, "not_found", "no such endpoint");
    next(false);
  }

  static std::string _source_key(const std::string& remote) { return "source:" + remote; }

  // What a 403 says was missing. Role names are documented vocabulary and safe to name.
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
  // Path parameters arrive percent-encoded: alice%40example.com is the one user
  // alice@example.com.
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
  std::shared_ptr<Throttle> _throttle;
  std::vector<Route> _routes;
  std::shared_ptr<StreamRegistry> _streams = std::make_shared<StreamRegistry>();
  std::shared_ptr<SubscriberAuth> _subscriber_auth;

  void _handle_subscriber(const Route& route, const http::request<http::string_body>& request, const std::string& target, RouteContext context,
                          std::function<void(bool)> next) {
    if (!_subscriber_auth) {
      write_error(context.response, http::status::not_found, "not_found", "no such route");
      return next(false);
    }

    const auto realm = context.parameter("realm");
    const auto authorization = std::string(request[http::field::authorization]);

    _subscriber_auth->check(
        realm, std::string(http::to_string(request.method())), target, authorization,
        [throttle = _throttle, context, handler = route.handler](SubscriberAuth::Outcome outcome) mutable {
          switch (outcome.kind) {
            case SubscriberAuth::Outcome::Kind::unavailable:
              write_error(context.response, http::status::service_unavailable, "unavailable", "the server cannot check credentials at the moment");
              return context.done();

            case SubscriberAuth::Outcome::Kind::no_realm:
              write_error(context.response, http::status::not_found, "not_found", "no such realm: " + context.parameter("realm"));
              return context.done();

            case SubscriberAuth::Outcome::Kind::challenge:
              // Limited as an open route is, by source address, so a password cannot be guessed at speed.
              if (const auto wait = throttle->take(_source_key(context.remote), throttle->limits().open)) {
                write_too_many(context.response, *wait);
                return context.done();
              }
              for (const auto& challenge : outcome.challenges) context.response->insert(http::field::www_authenticate, challenge);
              write_error(context.response, http::status::unauthorized, "unauthorized", "a subscriber's Digest credentials are required");
              return context.done();

            case SubscriberAuth::Outcome::Kind::ok:
              if (const auto wait = throttle->take("subscriber:" + outcome.subscriber->identity->uri->to_string(), throttle->limits().session)) {
                write_too_many(context.response, *wait);
                return context.done();
              }
              context.subscriber = std::move(outcome.subscriber);
              return handler(std::move(context));
          }
        });
  }
};

}  // namespace athenasip::api
