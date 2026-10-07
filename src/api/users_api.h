//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <string>

#include "../datastores/datastore.h"
#include "../loggers/logger.h"
#include "../types/password.h"
#include "../types/user.h"
#include "router.h"
#include "sessions.h"

namespace athenasip::api {

// Administers the users of this API: a different population from the subscribers
// ProvisioningAPI places in realms (docs/authentication.md).
//
// Everything needs `manage-admin-users` except changing a password, which any
// authenticated caller may do to their own by presenting the old one; that route decides
// for itself.
class UsersAPI : public std::enable_shared_from_this<UsersAPI> {
 public:
  // The iteration count is a parameter so a test can lower it. It is stored with each hash,
  // so raising it invalidates nobody.
  UsersAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor,
           std::shared_ptr<Sessions> sessions, std::uint32_t password_iterations = types::Password::default_iterations);

  void register_routes(Router& router);

 private:
  void _list(RouteContext context);
  void _create(RouteContext context);
  void _get(RouteContext context);
  void _update(RouteContext context);
  void _delete(RouteContext context);
  void _set_password(RouteContext context);
  void _revoke_sessions(RouteContext context);

  // Looks up the user a request names, or answers 404. The datastore does not distinguish
  // a missing user on delete, so the routes check here.
  void _with_user(const std::string& username, RouteContext context, std::function<void(std::shared_ptr<types::User>, RouteContext)> then);

  // Whether this request is that user acting on itself.
  static bool _is_self(const RouteContext& context, const std::string& username);

  // Rejects roles this node does not know. The datastore keeps unknown roles rather than
  // dropping them, so this is the only place a typo is caught.
  static bool _roles_are_known(const boost::json::array& roles, std::string& unknown);

  static boost::json::object _user_json(const types::User& user);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  std::shared_ptr<Sessions> _sessions;
  std::uint32_t _password_iterations;
};

}  // namespace athenasip::api
