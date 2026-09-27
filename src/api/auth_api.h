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
// `/auth/login` is how a credential is obtained and so cannot require one. `/auth/logout`
// and `/session` do require one, but not one the router can check: the router resolves
// configuration tokens and knows nothing yet about a session token, so these two ask
// `Sessions` themselves and answer 401 when nothing they were given resolves. Roles on
// the routes, and a router that resolves both kinds of credential, are the next step.
class AuthAPI : public std::enable_shared_from_this<AuthAPI> {
 public:
  AuthAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Sessions> sessions, std::shared_ptr<BearerAuth> auth);

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
  std::shared_ptr<BearerAuth> _auth;
};

}  // namespace athenasip::api
