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
#include "../types/location.h"
#include "../types/realm.h"
#include "../types/subscriber.h"
#include "router.h"

namespace athenasip::api {

// Provisioning: realms, subscribers, and a read of what is registered. It goes to the
// datastore directly on the API's own executor and never touches the Core strand, so an
// admin request cannot hold up a call.
class ProvisioningAPI : public std::enable_shared_from_this<ProvisioningAPI> {
 public:
  ProvisioningAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor,
                  std::shared_ptr<Config> config, std::string version);

  // Everything under /api/v1, with what each route requires of its caller.
  void register_routes(Router& router);

  // The other nodes, as they describe themselves on the event bus, and their heartbeat
  // interval. Without it the node list is this node alone.
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

  // Subscribers, always inside a realm.
  void _subscriber_list(RouteContext context);
  void _subscriber_create(RouteContext context);
  void _subscriber_get(RouteContext context);
  void _subscriber_update(RouteContext context);
  void _subscriber_delete(RouteContext context);

  // Registrations, read only: only a REGISTER writes a binding.
  void _registration_list(RouteContext context);

  void _health(RouteContext context);

  // What a web client needs to place a call: where to signal, and what to use for ICE.
  void _client_config(RouteContext context);

  // The cluster as this node knows it, which tells a client where else it could go.
  void _node_list(RouteContext context);
  // This node, then the others as they last described themselves; only those that are up
  // and current when usable_only.
  boost::json::array _nodes_json(bool usable_only) const;

  // Looks up the realm a request names, or answers 404.
  void _with_realm(const std::string& realm_name, RouteContext context, std::function<void(std::shared_ptr<types::Realm>, RouteContext)> then);

  // Answers for a failed operation. The contract does not say why a write failed, so a
  // failed create checks whether the thing already exists: 409 rather than 500.
  void _fail(RouteContext context, const plugins::Status& status, std::string code, std::string message, http::status http_status);

  boost::json::object _realm_json(const types::Realm& realm) const;
  static boost::json::object _subscriber_json(const types::Subscriber& subscriber);
  static boost::json::object _location_json(const types::Location& location, const std::string& uri);

  // Every enabled SIP transport, as a URI a client could use.
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
