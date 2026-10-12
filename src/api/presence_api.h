//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "api/router.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"

namespace athenasip {
class Core;
}  // namespace athenasip

namespace athenasip::api {

// Presence for the console (docs/design/presence.md, The API and the console): who is watching whom across the
// cluster, and each subscriber's state as its watchers are told it. Both are read from the datastore through the
// notifier, on the Core strand.
class PresenceAPI : public std::enable_shared_from_this<PresenceAPI> {
 public:
  PresenceAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor);

  void register_routes(Router& router);

 private:
  void _subscriptions(RouteContext context);
  void _state(RouteContext context);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;
  plugins::Executor _executor;
};

}  // namespace athenasip::api
