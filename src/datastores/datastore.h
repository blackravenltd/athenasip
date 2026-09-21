//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <ctime>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"
#include "../types/location.h"
#include "../types/realm.h"
#include "../types/sip_identity.h"
#include "../types/sip_uri.h"
#include "../types/subscriber.h"
#include "../types/url.h"

namespace athenasip::datastores {

// Every operation completes through a handler rather than returning, and the handler
// runs on the executor the caller passed in. That is the whole point: Core runs on one
// strand, a datastore is a network round trip, and a blocking read on the strand stops
// every call on the node rather than just the one that asked.
//
// This is also why it is async in version 1 of the contract. The interface is the
// plugin contract, so it cannot be made async later without breaking every plugin
// written against it.
//
// A read that finds nothing succeeds with an empty value. "No such subscriber" and
// "the datastore is unreachable" are different answers and callers act on them
// differently: one is a 404, the other a 500.
class Datastore : public plugins::Plugin {
 public:
  ~Datastore() override = default;

  std::string kind() const final { return plugins::kinds::datastore; }

  // Lifecycle. connect() is a round trip and is async like everything else; close() is
  // teardown, synchronous and safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // Realms. create and update are separate so provisioning can tell "already exists"
  // from "changed", which the admin API needs to answer 409 rather than overwrite.
  virtual void realm_get_by_name(plugins::Executor on, std::string realm_name, plugins::Handler<std::shared_ptr<types::Realm>> handler) = 0;
  virtual void realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) = 0;
  virtual void realm_update(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) = 0;
  virtual void realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) = 0;
  virtual void realm_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Realm>>> handler) = 0;

  // Subscribers.
  virtual void subscriber_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                              plugins::Handler<std::shared_ptr<types::Subscriber>> handler) = 0;
  virtual void subscriber_create(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) = 0;
  virtual void subscriber_update(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) = 0;
  virtual void subscriber_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) = 0;
  virtual void subscriber_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Subscriber>>> handler) = 0;

  // Registrations (RFC 3261 section 10 bindings). The binding carries what the node knows
  // about it that the Contact URI does not say: the RFC 3327 Path recorded at
  // registration, the flow it was learned over (RFC 5626) and which node holds that flow.
  // Its registered_at and expires_at are the store's to fill from expires_seconds, which
  // is the lifetime the registrar negotiated with the client and told the client about.
  //
  // A struct rather than a growing parameter list: the binding is one thing, and the
  // fields a cluster needs are exactly the ones a single node leaves empty.
  virtual void subscriber_register(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, types::Location binding, std::uint32_t expires_seconds,
                                   plugins::StatusHandler handler) = 0;
  virtual void subscriber_unregister(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact,
                                     plugins::StatusHandler handler) = 0;

  // Every live binding for a subscriber. Target determination needs all of them
  // (RFC 3261 16.5). Expired bindings are not returned.
  virtual void location_list(plugins::Executor on, std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) = 0;

  virtual void nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) = 0;
  virtual void nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) = 0;

  // Calls.
  virtual void call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;
  virtual void call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;
  virtual void call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) = 0;
  virtual void call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) = 0;

  // Registration and lookup go through the one PluginRegistry; these are the typed
  // way in for callers that know they want a datastore.
  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Datastore, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), plugins::kinds::datastore, std::move(scheme));
  }

  static std::shared_ptr<Datastore> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<Datastore>(std::move(logger), plugins::kinds::datastore, url_string);
  }
};

}  // namespace athenasip::datastores
