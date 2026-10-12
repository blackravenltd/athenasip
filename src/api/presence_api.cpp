//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "presence_api.h"

#include <utility>

#include "../core.h"
#include "../loggers/logger_scoped.h"
#include "../notifier.h"
#include "../types/user.h"
#include "api_json.h"

namespace athenasip::api {

PresenceAPI::PresenceAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor)
    : _logger(std::make_shared<loggers::LoggerScoped>("presence_api", std::move(logger))), _core(core), _executor(std::move(executor)) {}

void PresenceAPI::register_routes(Router& router) {
  using types::roles::manage_realm_subscribers;
  using types::roles::view_cluster_status;
  auto self = shared_from_this();

  // Who watches whom is the cluster's live state, as registrations are.
  router.add(http::verb::get, "/api/v1/presence/subscriptions", {view_cluster_status}, [self](RouteContext c) { self->_subscriptions(std::move(c)); });

  // A subscriber's state is the realm's, as the subscriber is.
  router.add(http::verb::get, "/api/v1/realms/{realm}/subscribers/{user}/presence", {manage_realm_subscribers},
             [self](RouteContext c) { self->_state(std::move(c)); });
}

void PresenceAPI::_subscriptions(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  core->post([core, context]() mutable {
    core->notifier()->subscriptions([context](plugins::Result<std::vector<Notifier::Watch>> found) mutable {
      if (!found.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", found.error);
        return context.done();
      }

      boost::json::array out;
      for (const auto& watch : found.value) {
        boost::json::object entry;
        entry["id"] = watch.id;
        entry["watcher"] = watch.watcher;
        entry["presentity"] = watch.presentity;
        entry["package"] = watch.package;
        entry["node_id"] = watch.node_id;
        entry["expires_at"] = static_cast<std::int64_t>(watch.expires_at);
        out.push_back(std::move(entry));
      }
      write_json(context.response, http::status::ok, out);
      context.done();
    });
  });
}

void PresenceAPI::_state(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  // Off, the node is no notifier, and a state it composed would be one no watcher is sent.
  if (!core->config->presence_enable || !core->datastore || !core->datastore->supports_presence()) {
    write_error(context.response, http::status::not_found, "presence_off", "this node does not serve presence");
    return context.done();
  }

  const auto aor = "sip:" + context.parameter("user") + "@" + context.parameter("realm");

  core->post([core, context, aor]() mutable {
    core->subscriber_get(std::make_shared<types::SIPIdentity>(aor), [core, context, aor](plugins::Result<std::shared_ptr<types::Subscriber>> found) mutable {
      if (!found.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", found.error);
        return context.done();
      }
      if (!found.value) {
        write_error(context.response, http::status::not_found, "not_found", "no such subscriber");
        return context.done();
      }

      core->notifier()->state(aor, [context, aor](plugins::Result<Notifier::State> state) mutable {
        if (!state.ok) {
          write_error(context.response, http::status::internal_server_error, "datastore_error", state.error);
          return context.done();
        }

        boost::json::object out;
        out["subscriber"] = aor;
        out["basic"] = state.value.basic;
        out["source"] = state.value.source;
        out["document"] = state.value.document;

        boost::json::array publications;
        for (const auto& record : state.value.publications) {
          boost::json::object entry;
          entry["etag"] = record.id;
          entry["content_type"] = record.content_type;
          entry["expires_at"] = static_cast<std::int64_t>(record.expires_at);
          entry["document"] = record.body;
          publications.push_back(std::move(entry));
        }
        out["publications"] = std::move(publications);

        boost::json::array dialogs;
        for (const auto& dialog : state.value.dialogs) {
          boost::json::object entry;
          entry["id"] = dialog.id;
          entry["call_id"] = dialog.call_id;
          entry["state"] = dialog.state;
          entry["direction"] = dialog.direction;
          entry["local"] = dialog.local;
          entry["remote"] = dialog.remote;
          dialogs.push_back(std::move(entry));
        }
        out["dialogs"] = std::move(dialogs);

        write_json(context.response, http::status::ok, out);
        context.done();
      });
    });
  });
}

}  // namespace athenasip::api
