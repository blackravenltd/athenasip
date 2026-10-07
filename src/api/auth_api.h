//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <utility>

#include "../loggers/logger.h"
#include "bearer_auth.h"
#include "router.h"
#include "sessions.h"

namespace athenasip::api {

// Login, logout and "who am I": where api::Sessions meets HTTP.
//
// /auth/login and /auth/logout are open routes. Login is how a credential is obtained;
// logout answers the same whether or not its token resolved, so an unknown token must
// reach the handler rather than a 401. /session requires a credential and no role.
class AuthAPI : public std::enable_shared_from_this<AuthAPI> {
 public:
  AuthAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Sessions> sessions);

  void register_routes(Router& router);

 private:
  void _login(RouteContext context);
  void _logout(RouteContext context);
  void _session(RouteContext context);

  // The single answer for an unknown username, a wrong password and a disabled user.
  static void _refuse(RouteContext& context);

  // The store could not be asked: 503, a retry rather than a defect. The reason goes to the
  // log, not to a body an unauthenticated caller reads.
  void _unavailable(RouteContext& context, const std::string& reason);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<Sessions> _sessions;
  std::shared_ptr<Throttle> _throttle;
};

}  // namespace athenasip::api
