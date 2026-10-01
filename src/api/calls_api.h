//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <memory>
#include <string>

#include "api/router.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"

namespace athenasip {
class Core;
class Call;
}  // namespace athenasip

namespace athenasip::api {

// What the node is doing now, rather than what it is configured with: the live calls and
// what the media engine carried for each, the engine itself, and the counters a monitoring
// system scrapes. Everything here is read from Core's own state, which belongs to the Core
// strand, so each request snapshots there and answers on the API's executor - an admin
// reading calls never holds up a call being set up.
class CallsAPI : public std::enable_shared_from_this<CallsAPI> {
 public:
  CallsAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor);

  void register_routes(Router& router);

 private:
  void _list(RouteContext context);
  void _get(RouteContext context);
  void _media(RouteContext context);
  void _metrics(RouteContext context);

  // The calls given, each with its media as the engine reports it, in the order given.
  // Runs on the strand and answers on the executor.
  void _describe(std::vector<std::shared_ptr<Call>> calls, std::function<void(boost::json::array)> then);

  static boost::json::object _call_json(const Call& call);
  static boost::json::value _media_json(const std::string& document, const std::string& engine);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;
  plugins::Executor _executor;
};

}  // namespace athenasip::api
