//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "security_api.h"

#include <boost/json.hpp>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "../auth_guard.h"
#include "../core.h"
#include "../types/sip_uri.h"
#include "../types/user.h"
#include "../util.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

boost::json::string time_json(std::chrono::system_clock::time_point when) { return boost::json::string(Util::to_iso8601(when)); }

// `field` names the key: source for a ban, aor for a closure.
boost::json::array held_json(const std::vector<AuthGuard::Held>& held, const char* field) {
  boost::json::array out;
  const auto now = std::chrono::system_clock::now();
  for (const auto& one : held) {
    out.push_back(boost::json::object{{field, one.key},
                                      {"node", one.node},
                                      {"since", time_json(one.since)},
                                      {"until", time_json(one.until)},
                                      {"seconds_left", std::chrono::ceil<std::chrono::seconds>(one.until - now).count()}});
  }
  return out;
}

// An AOR as the guard keys it (SIPUri::canonical_address), however the request spelt it.
std::string aor_of(const std::string& text) {
  const types::SIPUri uri(text);
  return uri.valid ? uri.canonical_address() : text;
}

}  // namespace

SecurityAPI::SecurityAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor)
    : _logger(std::make_shared<loggers::LoggerScoped>("security_api", std::move(logger))), _core(core), _executor(std::move(executor)) {}

void SecurityAPI::register_routes(Router& router) {
  using types::roles::manage_cluster;
  using types::roles::view_cluster_status;
  auto self = shared_from_this();

  // Reading what is in force is status; lifting it changes the cluster.
  router.add(http::verb::get, "/api/v1/security/bans", {view_cluster_status}, [self](RouteContext c) { self->_bans(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/security/bans/{source}", {manage_cluster}, [self](RouteContext c) { self->_lift(std::move(c)); });
  router.add(http::verb::get, "/api/v1/security/closed-aors", {view_cluster_status}, [self](RouteContext c) { self->_closed(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/security/closed-aors/{aor}", {manage_cluster}, [self](RouteContext c) { self->_reopen(std::move(c)); });
}

void SecurityAPI::_bans(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is stopping");
    return context.done();
  }
  write_json(context.response, http::status::ok, held_json(core->auth_guard().bans(), "source"));
  context.done();
}

void SecurityAPI::_closed(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is stopping");
    return context.done();
  }
  write_json(context.response, http::status::ok, held_json(core->auth_guard().closures(), "aor"));
  context.done();
}

void SecurityAPI::_lift(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is stopping");
    return context.done();
  }

  // Lifted on every node whether or not this one held it: a node that started after the ban never heard of it, and
  // the others did.
  const auto source = context.parameter("source");
  const bool held = core->auth_lift(source);
  _logger->info("Ban on " + source + " lifted over the admin API" + (held ? "" : ", though this node held none"));

  if (!held) {
    write_error(context.response, http::status::not_found, "not_found", source + " is not banned here; the lift was sent to the other nodes");
  } else {
    write_json(context.response, http::status::ok, boost::json::object{{"source", source}});
  }
  context.done();
}

void SecurityAPI::_reopen(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is stopping");
    return context.done();
  }

  const auto aor = aor_of(context.parameter("aor"));
  const bool held = core->auth_reopen(aor);
  _logger->info(aor + " reopened over the admin API" + (held ? "" : ", though this node held it open"));

  if (!held) {
    write_error(context.response, http::status::not_found, "not_found", aor + " is not closed here; the reopening was sent to the other nodes");
  } else {
    write_json(context.response, http::status::ok, boost::json::object{{"aor", aor}});
  }
  context.done();
}

}  // namespace athenasip::api
