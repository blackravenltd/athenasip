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

// Logging in, logging out, and asking who you are.
//
// The three routes that turn a password into a token and a token back into an identity.
// `api::Sessions` does the work; this is where it meets HTTP, which means it is where the
// status codes are decided and where the wording of a refusal is settled.
//
// Two of these routes are declared open, which is deliberate rather than an oversight.
// `/auth/login` is how a credential is obtained and so cannot require one, and
// `/auth/logout` answers the same whether or not the token it was handed resolved, so a
// token that resolves to nothing has to reach the handler rather than be turned away with
// a 401 that tells the caller it was not real. `/session` requires a credential and names
// no roles, so any authenticated caller reaches it and the router has resolved who they
// are before it runs.
class AuthAPI : public std::enable_shared_from_this<AuthAPI> {
 public:
  AuthAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Sessions> sessions);

  void register_routes(Router& router);

 private:
  void _login(RouteContext context);
  void _logout(RouteContext context);
  void _session(RouteContext context);

  // The one answer a caller gets for a username that does not exist, a password that
  // does not verify and a user that has been disabled. It is here, once, because three
  // handlers writing "the same" refusal separately is how they come to differ.
  static void _refuse(RouteContext& context);

  // The store could not be asked, which is neither a bad password nor a bug in the
  // caller. 503 rather than the 500 the provisioning routes answer, because a dependency
  // being down is a retry rather than a defect, and because the detail belongs in the log
  // rather than in a body an unauthenticated caller reads.
  void _unavailable(RouteContext& context, const std::string& reason);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<Sessions> _sessions;
};

}  // namespace athenasip::api
