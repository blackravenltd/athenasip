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

// Administering the users that administer this node.
//
// A different population from the subscribers `ProvisioningAPI` places in realms: a user
// holds roles and calls this API, a subscriber holds an HA1 and makes telephone calls.
// Neither is created from the other and neither credential works as the other - see
// docs/authentication.md.
//
// Everything here needs `manage-admin-users` except changing a password, which anyone may
// do to their own by presenting the old one. That route is therefore declared for any
// authenticated caller and decides for itself, because "it is mine" is not something a
// role can express.
class UsersAPI : public std::enable_shared_from_this<UsersAPI> {
 public:
  // The iteration count is a parameter rather than a constant so a test can turn it down.
  // It is deliberately not configuration: nothing has asked to tune it, the count is
  // stored with each hash so it can be raised later without invalidating anybody, and a
  // knob whose wrong setting is invisible until somebody steals the database is not one to
  // offer before there is a reason.
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

  // The user a request names, or a 404 that says so. Every route below the collection
  // starts here, exactly as `_with_realm` does for realms: a user that does not exist is a
  // typo rather than a 500, and the datastore deliberately will not make that distinction
  // for a delete.
  void _with_user(const std::string& username, RouteContext context, std::function<void(std::shared_ptr<types::User>, RouteContext)> then);

  // Whether this request is that user acting on itself.
  static bool _is_self(const RouteContext& context, const std::string& username);

  // What may be granted, checked on the way in. The datastore carries a role it does not
  // recognise rather than dropping it, because an older node rewriting a user must not
  // silently strip a role a newer one gave them - which leaves this as the only place a
  // typo can be caught.
  static bool _roles_are_known(const boost::json::array& roles, std::string& unknown);

  static boost::json::object _user_json(const types::User& user);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  std::shared_ptr<Sessions> _sessions;
  std::uint32_t _password_iterations;
};

}  // namespace athenasip::api
