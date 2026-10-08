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
#include "datastores/datastore.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"
#include "trunk_registrar.h"
#include "types/trunk.h"

namespace athenasip::api {

// /api/v1/trunks: the carriers and PBXs calls leave by and arrive from (types/trunk.h). manage-trunks for all of
// it. A trunk's password is written and never read back, as a subscriber's is.
class TrunksAPI : public std::enable_shared_from_this<TrunksAPI> {
 public:
  TrunksAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor);

  void register_routes(Router& router);

  // Where the registrations the nodes report are read from. Without it a trunk shows none.
  void statuses_register(std::shared_ptr<TrunkStatuses> statuses) { _statuses = std::move(statuses); }

  // A trunk as the API shows it: everything but the password, and whether one is set, and its registration as
  // the node holding it last reported it.
  boost::json::object to_json(const types::Trunk& trunk) const;

  // Applies a request body to a trunk: on a create every field, on an update only those given. Empty when it
  // applied, else what is wrong, for a 400.
  static std::string apply(const boost::json::object& body, types::Trunk& trunk, bool creating);

 private:
  void _list(RouteContext context);
  void _create(RouteContext context);
  void _get(RouteContext context);
  void _update(RouteContext context);
  void _delete(RouteContext context);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  std::shared_ptr<TrunkStatuses> _statuses;
};

}  // namespace athenasip::api
