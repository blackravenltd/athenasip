//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "lua_policy.h"

#include <yaml-cpp/yaml.h>

#include <utility>

#include "../config.h"
#include "../loggers/logger_scoped.h"
#include "../script/lua_library.h"

namespace athenasip::policy {

namespace {

using script::LuaEngine;

// RFC 3261 21: the reason phrase for a code a script gave without one.
std::string reason_for(std::uint16_t code) {
  switch (code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 407:
      return "Proxy Authentication Required";
    case 408:
      return "Request Timeout";
    case 410:
      return "Gone";
    case 480:
      return "Temporarily Unavailable";
    case 484:
      return "Address Incomplete";
    case 486:
      return "Busy Here";
    case 487:
      return "Request Terminated";
    case 488:
      return "Not Acceptable Here";
    case 500:
      return "Server Internal Error";
    case 503:
      return "Service Unavailable";
    case 600:
      return "Busy Everywhere";
    case 603:
      return "Decline";
    case 604:
      return "Does Not Exist Anywhere";
    default:
      break;
  }
  if (code >= 600) return "Global Failure";
  if (code >= 500) return "Server Error";
  if (code >= 400) return "Request Failure";
  if (code >= 300) return "Redirection";
  return "OK";
}

// A field of the table at idx, as text; empty when it is absent or not a string.
std::string text_field(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return out;
}

std::optional<lua_Integer> integer_field(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  std::optional<lua_Integer> out;
  if (lua_isinteger(L, -1)) out = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return out;
}

std::optional<bool> boolean_field(lua_State* L, int idx, const char* key) {
  lua_getfield(L, idx, key);
  std::optional<bool> out;
  if (lua_isboolean(L, -1)) out = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return out;
}

std::uint16_t code_of(lua_State* L, int idx) { return static_cast<std::uint16_t>(integer_field(L, idx, "code").value_or(500)); }

std::string reason_of(lua_State* L, int idx, std::uint16_t code) {
  auto reason = text_field(L, idx, "reason");
  return reason.empty() ? reason_for(code) : reason;
}

// One target, as athenasip.route.subscriber and athenasip.route.uri make it.
std::optional<Target> target_at(lua_State* L, int idx, std::string& error) {
  if (!lua_istable(L, idx)) {
    error = "a target is a table from athenasip.route.subscriber or athenasip.route.uri";
    return std::nullopt;
  }

  const auto kind = text_field(L, idx, "target");

  if (kind == "subscriber") {
    lua_getfield(L, idx, "subscriber");
    auto subscriber = script::to_subscriber(L, -1);
    lua_pop(L, 1);
    if (!subscriber) {
      error = "a subscriber target names no subscriber";
      return std::nullopt;
    }

    lua_getfield(L, idx, "bindings");
    auto bindings = script::to_bindings(L, -1);
    lua_pop(L, 1);

    std::optional<std::vector<types::Location>> read;
    if (bindings) read = *bindings;
    auto target = Target::of(std::move(subscriber), std::move(read));
    if (const auto ring = integer_field(L, idx, "ring_timeout"); ring && *ring > 0) target.ring_timeout = std::chrono::seconds(*ring);
    return target;
  }

  if (kind == "uri") {
    lua_getfield(L, idx, "uri");
    auto uri = script::to_uri(L, -1);
    lua_pop(L, 1);
    if (!uri) {
      error = "a uri target has no URI that parses";
      return std::nullopt;
    }

    lua_getfield(L, idx, "next_hop");
    std::shared_ptr<types::SIPUri> next_hop;
    if (!lua_isnil(L, -1)) {
      next_hop = script::to_uri(L, -1);
      if (!next_hop) {
        lua_pop(L, 1);
        error = "a uri target's next_hop does not parse";
        return std::nullopt;
      }
    }
    lua_pop(L, 1);
    auto target = Target::to(std::move(uri), std::move(next_hop), text_field(L, idx, "trunk"));
    if (const auto ring = integer_field(L, idx, "ring_timeout"); ring && *ring > 0) target.ring_timeout = std::chrono::seconds(*ring);
    return target;
  }

  error = "a target is a table from athenasip.route.subscriber or athenasip.route.uri";
  return std::nullopt;
}

std::optional<std::vector<Target>> targets_at(lua_State* L, int idx, std::string& error) {
  std::vector<Target> targets;
  const auto count = luaL_len(L, idx);

  for (lua_Integer i = 1; i <= count; ++i) {
    lua_geti(L, idx, i);
    auto target = target_at(L, lua_gettop(L), error);
    lua_pop(L, 1);

    if (!target) {
      error = "target " + std::to_string(i) + ": " + error;
      return std::nullopt;
    }
    targets.push_back(std::move(*target));
  }
  return targets;
}

// {media_anchor =, media_profile =}, as athenasip.config.behaviour and a realm's behaviour_effective give it, over
// the server's defaults.
std::optional<types::MediaPolicy> media_at(lua_State* L, int idx, const types::MediaPolicy& server, std::string& error) {
  if (!lua_istable(L, idx)) return std::nullopt;

  auto media = server;
  if (const auto anchor = boolean_field(L, idx, "media_anchor")) media.anchor = *anchor;

  if (const auto profile = text_field(L, idx, "media_profile"); !profile.empty()) {
    const auto parsed = types::MediaPolicy::parse_profiles(profile);
    if (!parsed) {
      error = "media_profile \"" + profile + "\" is none of transport, mirror, rtp, webrtc, srtp";
      return std::nullopt;
    }
    media.profiles = *parsed;
  }
  return media;
}

template <typename T>
plugins::Result<T> refused(const std::string& hook, const std::string& why) {
  return plugins::Result<T>::failure(hook + "() returned " + why);
}

plugins::Result<AuthDecision> auth_from(lua_State* L, int results) {
  if (results < 1 || !lua_istable(L, -results))
    return refused<AuthDecision>("authorize", "no decision: return athenasip.auth.accept(), .digest{} or .reject()");

  const int idx = lua_absindex(L, -results);
  if (text_field(L, idx, "decision") != "auth") return refused<AuthDecision>("authorize", "something other than an athenasip.auth decision");

  const auto kind = text_field(L, idx, "kind");
  if (kind == "accept") return plugins::Result<AuthDecision>::success(AuthDecision::accept());

  if (kind == "digest") {
    lua_getfield(L, idx, "realm");
    auto realm = script::to_realm(L, -1);
    lua_pop(L, 1);
    return plugins::Result<AuthDecision>::success(AuthDecision::digest(std::move(realm), boolean_field(L, idx, "from_must_match").value_or(true)));
  }

  if (kind == "trusted") {
    const auto trunk = text_field(L, idx, "trunk");
    if (trunk.empty()) return refused<AuthDecision>("authorize", "a trusted decision with no trunk");
    return plugins::Result<AuthDecision>::success(AuthDecision::trusted(trunk));
  }

  if (kind == "reject") {
    const auto code = code_of(L, idx);
    return plugins::Result<AuthDecision>::success(AuthDecision::reject(code, reason_of(L, idx, code)));
  }

  return refused<AuthDecision>("authorize", "an athenasip.auth decision of no kind it knows");
}

plugins::Result<RouteDecision> route_from(lua_State* L, int results, const Config& config) {
  // Nothing at all is nowhere to go.
  if (results < 1 || lua_isnil(L, -results)) return plugins::Result<RouteDecision>::success(RouteDecision::forward({}));
  if (!lua_istable(L, -results)) return refused<RouteDecision>("route", "something other than an athenasip.route decision or a list of targets");

  const int idx = lua_absindex(L, -results);
  const auto decision = text_field(L, idx, "decision");
  std::string error;

  // A bare list of targets.
  if (decision.empty()) {
    auto targets = targets_at(L, idx, error);
    if (!targets) return refused<RouteDecision>("route", error);
    return plugins::Result<RouteDecision>::success(RouteDecision::forward(std::move(*targets)));
  }

  if (decision != "route") return refused<RouteDecision>("route", "something other than an athenasip.route decision");

  const auto kind = text_field(L, idx, "kind");
  if (kind == "reply") {
    const auto code = code_of(L, idx);
    return plugins::Result<RouteDecision>::success(RouteDecision::reply(code, reason_of(L, idx, code)));
  }

  if (kind != "forward") return refused<RouteDecision>("route", "an athenasip.route decision of no kind it knows");

  lua_getfield(L, idx, "targets");
  auto targets = lua_istable(L, -1) ? targets_at(L, lua_gettop(L), error) : std::optional<std::vector<Target>>(std::vector<Target>{});
  lua_pop(L, 1);
  if (!targets) return refused<RouteDecision>("route", error);

  auto out = RouteDecision::forward(std::move(*targets));

  lua_getfield(L, idx, "media");
  out.media = media_at(L, lua_gettop(L), config.behaviour, error);
  lua_pop(L, 1);
  if (!error.empty()) return refused<RouteDecision>("route", error);

  out.rewrite_contact = boolean_field(L, idx, "rewrite_contact");
  return plugins::Result<RouteDecision>::success(std::move(out));
}

plugins::Result<FailureDecision> failure_from(lua_State* L, int results) {
  if (results < 1 || lua_isnil(L, -results)) return plugins::Result<FailureDecision>::success(FailureDecision::next());

  if (lua_type(L, -results) == LUA_TSTRING) {
    const std::string answer = lua_tostring(L, -results);
    if (answer == "next") return plugins::Result<FailureDecision>::success(FailureDecision::next());
    if (answer == "stop") return plugins::Result<FailureDecision>::success(FailureDecision::stop());
    return refused<FailureDecision>("on_failure", "\"" + answer + "\", which is neither \"next\" nor \"stop\"");
  }

  if (!lua_istable(L, -results)) return refused<FailureDecision>("on_failure", "something other than \"next\", \"stop\" or a list of targets");

  std::string error;
  auto targets = targets_at(L, lua_absindex(L, -results), error);
  if (!targets) return refused<FailureDecision>("on_failure", error);
  return plugins::Result<FailureDecision>::success(FailureDecision::next(std::move(*targets)));
}

plugins::Result<RegisterDecision> register_from(lua_State* L, int results) {
  if (results < 1 || !lua_istable(L, -results)) {
    return refused<RegisterDecision>("register", "no decision: return athenasip.register.accept{}, .forward() or .reject()");
  }

  const int idx = lua_absindex(L, -results);
  if (text_field(L, idx, "decision") != "register") return refused<RegisterDecision>("register", "something other than an athenasip.register decision");

  const auto kind = text_field(L, idx, "kind");
  if (kind == "forward") return plugins::Result<RegisterDecision>::success(RegisterDecision::forward());

  if (kind == "reject") {
    const auto code = code_of(L, idx);
    return plugins::Result<RegisterDecision>::success(RegisterDecision::reject(code, reason_of(L, idx, code)));
  }

  if (kind != "accept") return refused<RegisterDecision>("register", "an athenasip.register decision of no kind it knows");

  lua_getfield(L, idx, "realm");
  auto realm = script::to_realm(L, -1);
  lua_pop(L, 1);
  if (!realm) return refused<RegisterDecision>("register", "an accept with no realm");

  const auto count = [&](const char* key, lua_Integer fallback) -> std::uint32_t {
    const auto value = integer_field(L, idx, key).value_or(fallback);
    return value < 0 ? 0 : static_cast<std::uint32_t>(value);
  };

  return plugins::Result<RegisterDecision>::success(
      RegisterDecision::accept(std::move(realm), count("max_expires", 3600), count("min_expires", 0), count("qualify", 0)));
}

}  // namespace

LuaPolicy::LuaPolicy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("policy", logger)), _base_logger(std::move(logger)) {
  (void)url;
}

plugins::Settings LuaPolicy::settings() {
  using namespace plugins::define;
  return {
      section("lua", "The lua:// policy: routing and authorisation as Lua scripts (docs/scripting.md)."),
      list("lua.path", "Directories searched, in order, for the entry script and for require. The standard scripts are searched last."),
      text("lua.entry", "main.lua", "The script that defines the hooks. With none on the path, the standard one, which behaves as builtin://."),
      integer("lua.instruction_limit", "1000000", "Lua instructions one hook call may run, not counting time waiting on the store.", 1000),
      integer("lua.timeout_ms", "5000", "How long one hook call may take, waiting included, in milliseconds.", 1),
      integer("lua.memory_limit_mb", "64", "Megabytes the scripts may hold.", 1),
  };
}

bool LuaPolicy::configure(const YAML::Node& own_root, const Config& system) {
  std::vector<std::string> path;
  std::string entry = "main.lua";
  script::LuaEngine::Limits limits;

  try {
    if (own_root && own_root.IsMap()) {
      if (const auto paths = own_root["path"]) {
        if (paths.IsSequence()) {
          for (const auto& directory : paths) path.push_back(directory.as<std::string>());
        } else {
          path.push_back(paths.as<std::string>());
        }
      }
      if (own_root["entry"]) entry = own_root["entry"].as<std::string>();
      if (own_root["instruction_limit"]) limits.instructions = own_root["instruction_limit"].as<std::uint64_t>();
      if (own_root["timeout_ms"]) limits.timeout = std::chrono::milliseconds(own_root["timeout_ms"].as<std::uint64_t>());
      if (own_root["memory_limit_mb"]) limits.memory = own_root["memory_limit_mb"].as<std::size_t>() * 1024 * 1024;
    }
  } catch (const std::exception& e) {
    _error = std::string("policy.lua: ") + e.what();
    _logger->error(_error);
    return false;
  }

  _engine = std::make_shared<script::LuaEngine>(_base_logger, std::move(path), std::move(entry), limits);
  _engine->config_set(&system);

  if (auto error = _engine->load(); !error.empty()) {
    _error = std::move(error);
    _logger->error("The scripts did not load - " + _error);
    return false;
  }
  return true;
}

void LuaPolicy::attach(std::shared_ptr<Host> host) {
  Policy::attach(host);

  // A Core built without configure, as a test builds one, still gets the standard scripts.
  if (!_engine) {
    _engine = std::make_shared<script::LuaEngine>(_base_logger, std::vector<std::string>{}, "main.lua", script::LuaEngine::Limits{});
    if (const auto error = _engine->load(); !error.empty()) _logger->error("The standard scripts did not load - " + error);
  }
  _engine->attach(std::move(host));
}

void LuaPolicy::authorize(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<AuthDecision> handler) {
  _engine->call(
      on, "authorize", [request](lua_State* L, const std::shared_ptr<bool>& live) { return script::push_request(L, request, live), 1; },
      [on, handler](lua_State* L, int results, const std::string& error) {
        _complete(on, handler, error.empty() ? auth_from(L, results) : plugins::Result<AuthDecision>::failure(error));
      });
}

void LuaPolicy::route(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RouteDecision> handler) {
  auto host = _host;
  _engine->call(
      on, "route", [request](lua_State* L, const std::shared_ptr<bool>& live) { return script::push_request(L, request, live), 1; },
      [on, handler, host](lua_State* L, int results, const std::string& error) {
        _complete(on, handler, error.empty() ? route_from(L, results, host->config()) : plugins::Result<RouteDecision>::failure(error));
      });
}

void LuaPolicy::on_failure(plugins::Executor on, std::shared_ptr<RequestView> request, std::shared_ptr<SIPMessage> response, ForkState state,
                           plugins::Handler<FailureDecision> handler) {
  // A script with no on_failure goes on, as the RFC's proxy does.
  if (!_engine->defines("on_failure")) return Policy::on_failure(on, std::move(request), std::move(response), state, std::move(handler));

  _engine->call(
      on, "on_failure",
      [request, response, state](lua_State* L, const std::shared_ptr<bool>& live) {
        script::push_request(L, request, live);
        script::push_response(L, response, live);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, static_cast<lua_Integer>(state.tried));
        lua_setfield(L, -2, "tried");
        lua_pushinteger(L, static_cast<lua_Integer>(state.remaining));
        lua_setfield(L, -2, "remaining");
        lua_pushinteger(L, state.best);
        lua_setfield(L, -2, "best");
        return 3;
      },
      [on, handler](lua_State* L, int results, const std::string& error) {
        _complete(on, handler, error.empty() ? failure_from(L, results) : plugins::Result<FailureDecision>::failure(error));
      });
}

void LuaPolicy::register_(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RegisterDecision> handler) {
  _engine->call(
      on, "register", [request](lua_State* L, const std::shared_ptr<bool>& live) { return script::push_request(L, request, live), 1; },
      [on, handler](lua_State* L, int results, const std::string& error) {
        _complete(on, handler, error.empty() ? register_from(L, results) : plugins::Result<RegisterDecision>::failure(error));
      });
}

}  // namespace athenasip::policy
