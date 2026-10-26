//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "api/router.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "plugins/plugin.h"

namespace athenasip {
class Core;
}  // namespace athenasip

namespace athenasip::api {

// The bans and closed addresses of record that Digest guessing over SIP has put in force (AuthGuard,
// docs/authentication.md, Guessing), as every node holds them, and an administrator's lift of one, which reaches
// every node over the bus.
class SecurityAPI : public std::enable_shared_from_this<SecurityAPI> {
 public:
  SecurityAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor);

  void register_routes(Router& router);

 private:
  void _bans(RouteContext context);
  void _lift(RouteContext context);
  void _closed(RouteContext context);
  void _reopen(RouteContext context);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  plugins::Executor _executor;
};

}  // namespace athenasip::api
