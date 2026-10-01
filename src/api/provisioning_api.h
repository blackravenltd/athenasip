//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "../config.h"
#include "../datastores/datastore.h"
#include "../loggers/logger.h"
#include "../node_directory.h"
#include "../types/account.h"
#include "../types/location.h"
#include "../types/realm.h"
#include "router.h"

namespace athenasip::api {

// Provisioning: realms, accounts, and a read of what is currently registered.
//
// It goes to the datastore directly, on the API's own executor, and never touches the
// Core strand. That is what the async plugin contract is for: an admin listing accounts
// must not be able to hold up a call, and reaching Core for a datastore read would put
// every admin request on the call path. Only a read of Core's own registries - live
// calls, live channels - needs the strand, and nothing here does.
class ProvisioningAPI : public std::enable_shared_from_this<ProvisioningAPI> {
 public:
  ProvisioningAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor,
                  std::shared_ptr<Config> config, std::string version);

  // Everything under /api/v1. The scope each route needs is named here and nowhere
  // else.
  void register_routes(Router& router);

  // The other nodes, as they describe themselves on the event bus, and how often each says
  // so. Without it the node list is this node alone, which is what a single node is.
  void nodes_register(std::shared_ptr<NodeDirectory> nodes, std::chrono::seconds heartbeat) {
    _nodes = std::move(nodes);
    _node_heartbeat = heartbeat;
  }

 private:
  // Realms
  void _realm_list(RouteContext context);
  void _realm_create(RouteContext context);
  void _realm_get(RouteContext context);
  void _realm_update(RouteContext context);
  void _realm_delete(RouteContext context);

  // Accounts, always inside a realm: an account is only meaningful in one.
  void _account_list(RouteContext context);
  void _account_create(RouteContext context);
  void _account_get(RouteContext context);
  void _account_update(RouteContext context);
  void _account_delete(RouteContext context);

  // Registrations, read only. A binding is written by a REGISTER and by nothing else.
  void _registration_list(RouteContext context);

  void _health(RouteContext context);

  // Everything a web client needs to place a call that it cannot be told by hand: where
  // to signal, and what to use for ICE. A browser reads no configuration file.
  void _client_config(RouteContext context);

  // The cluster as this node knows it. One node today, because nothing discovers the
  // others yet; the shape is what the M4 discovery bus fills in, and it is what a client
  // reads to know where else it could go when this node stops answering.
  void _node_list(RouteContext context);

  // The realm a request names, or a 404 that says so. Every account route starts here,
  // because an account in a realm that does not exist is a typo rather than a 500.
  void _with_realm(const std::string& realm_name, RouteContext context, std::function<void(std::shared_ptr<types::Realm>, RouteContext)> then);

  // What the store said when an operation that returns nothing failed. The contract
  // reports "it did not happen" without saying why, so a create that failed asks
  // whether the thing is already there: that is the difference between 409 and 500.
  void _fail(RouteContext context, const plugins::Status& status, std::string code, std::string message, http::status http_status);

  boost::json::object _realm_json(const types::Realm& realm) const;
  static boost::json::object _account_json(const types::Account& account);
  static boost::json::object _location_json(const types::Location& location, const std::string& uri);

  // Every SIP transport this node has switched on, as a URI a client could use.
  boost::json::array _local_transports() const;

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  std::shared_ptr<Config> _config;
  std::string _version;
  std::shared_ptr<NodeDirectory> _nodes;
  std::chrono::seconds _node_heartbeat{30};
};

}  // namespace athenasip::api
