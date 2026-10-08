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
#include "../types/session.h"
#include "../types/sip_identity.h"
#include "../types/sip_uri.h"
#include "../types/subscriber.h"
#include "../types/trunk.h"
#include "../types/url.h"
#include "../types/user.h"

namespace athenasip::datastores {

// Every operation completes through a handler that runs on the executor the caller passed
// in. Core runs on one strand and a datastore is a network round trip, so nothing here
// may block.
//
// A read that finds nothing succeeds with an empty value: "no such subscriber" (404) and
// "the datastore is unreachable" (500) are different answers.
class Datastore : public plugins::Plugin {
 public:
  ~Datastore() override = default;

  std::string kind() const final { return plugins::kinds::datastore; }

  // connect() is a round trip and async; close() is synchronous and safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

 protected:
  // The answer for an operation a driver does not implement: a failure naming it.
  void _unsupported(plugins::Executor on, plugins::StatusHandler handler, const std::string& operation) {
    _complete(std::move(on), std::move(handler), plugins::Status::failure(name() + " does not support " + operation));
  }

  template <typename T>
  void _unsupported(plugins::Executor on, plugins::Handler<T> handler, const std::string& operation) {
    _complete(std::move(on), std::move(handler), plugins::Result<T>::failure(name() + " does not support " + operation));
  }

 public:
  // Realms. create and update are separate so the API can answer 409 rather than overwrite.
  virtual void realm_get_by_name(plugins::Executor on, std::string realm_name, plugins::Handler<std::shared_ptr<types::Realm>> handler) = 0;
  virtual void realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) = 0;
  virtual void realm_update(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) = 0;

  // Deletes the realm with every subscriber in it and their bindings. Fails, and keeps the
  // realm, if any of that could not be done.
  virtual void realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) = 0;
  virtual void realm_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Realm>>> handler) = 0;

  // Users and their sessions: who may use the API, as distinct from the subscribers
  // registered on a realm (docs/authentication.md). Optional: the defaults answer
  // "unsupported", and a deployment that wants admin logins needs a driver that implements
  // them.
  virtual void user_get(plugins::Executor on, std::string username, plugins::Handler<std::shared_ptr<types::User>> handler) {
    _unsupported<std::shared_ptr<types::User>>(std::move(on), std::move(handler), "user_get");
  }

  virtual void user_create(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
    (void)user;
    _unsupported(std::move(on), std::move(handler), "user_create");
  }

  virtual void user_update(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
    (void)user;
    _unsupported(std::move(on), std::move(handler), "user_update");
  }

  virtual void user_delete(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
    (void)username;
    _unsupported(std::move(on), std::move(handler), "user_delete");
  }

  virtual void user_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::User>>> handler) {
    _unsupported<std::vector<std::shared_ptr<types::User>>>(std::move(on), std::move(handler), "user_list");
  }

  // A session is stored under the hash of its token, never the token; the caller hashes.
  virtual void session_create(plugins::Executor on, types::Session session, plugins::StatusHandler handler) {
    (void)session;
    _unsupported(std::move(on), std::move(handler), "session_create");
  }

  virtual void session_get(plugins::Executor on, std::string token_hash, plugins::Handler<std::shared_ptr<types::Session>> handler) {
    (void)token_hash;
    _unsupported<std::shared_ptr<types::Session>>(std::move(on), std::move(handler), "session_get");
  }

  // Succeeds whether or not the hash was held, unlike the realm and subscriber deletes, so
  // that a logout cannot be used to probe whether a token is real. Failure means the store
  // could not do it and the session may still be live.
  virtual void session_delete(plugins::Executor on, std::string token_hash, plugins::StatusHandler handler) {
    (void)token_hash;
    _unsupported(std::move(on), std::move(handler), "session_delete");
  }

  // Deletes every session a user holds, so disabling or deleting a user takes effect at once.
  virtual void session_delete_for_user(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
    (void)username;
    _unsupported(std::move(on), std::move(handler), "session_delete_for_user");
  }

  // Trunks (types/trunk.h), by name without regard to case. Optional, as users are. create fails if the trunk
  // exists and update if it does not, so the API can answer 409 or 404.
  virtual void trunk_get(plugins::Executor on, std::string name, plugins::Handler<std::shared_ptr<types::Trunk>> handler) {
    (void)name;
    _unsupported<std::shared_ptr<types::Trunk>>(std::move(on), std::move(handler), "trunk_get");
  }

  virtual void trunk_create(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
    (void)trunk;
    _unsupported(std::move(on), std::move(handler), "trunk_create");
  }

  virtual void trunk_update(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
    (void)trunk;
    _unsupported(std::move(on), std::move(handler), "trunk_update");
  }

  virtual void trunk_delete(plugins::Executor on, std::string name, plugins::StatusHandler handler) {
    (void)name;
    _unsupported(std::move(on), std::move(handler), "trunk_delete");
  }

  virtual void trunk_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Trunk>>> handler) {
    _unsupported<std::vector<std::shared_ptr<types::Trunk>>>(std::move(on), std::move(handler), "trunk_list");
  }

  // A named lease, held by one holder at a time until it lapses: how one node of a cluster takes a job, such as
  // registering to a trunk. Answers whether `holder` holds it now; a holder that already did has it renewed for
  // `seconds`. Optional; a node without it does the job itself.
  virtual void lease(plugins::Executor on, std::string name, std::string holder, std::uint32_t seconds, plugins::Handler<bool> handler) {
    (void)name;
    (void)holder;
    (void)seconds;
    _unsupported<bool>(std::move(on), std::move(handler), "lease");
  }

  // Subscribers.
  virtual void subscriber_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                              plugins::Handler<std::shared_ptr<types::Subscriber>> handler) = 0;
  virtual void subscriber_create(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) = 0;
  virtual void subscriber_update(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) = 0;
  virtual void subscriber_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) = 0;
  virtual void subscriber_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Subscriber>>> handler) = 0;

  // Registrations (RFC 3261 section 10 bindings). The binding carries the RFC 3327 Path,
  // the flow it was learned over (RFC 5626) and the node holding that flow. The store fills
  // registered_at and expires_at from expires_seconds, the lifetime the registrar granted.
  virtual void subscriber_register(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, types::Location binding, std::uint32_t expires_seconds,
                                   plugins::StatusHandler handler) = 0;
  virtual void subscriber_unregister(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact,
                                     plugins::StatusHandler handler) = 0;

  // Every unexpired binding for a subscriber (RFC 3261 16.5 needs them all).
  virtual void location_list(plugins::Executor on, std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) = 0;

  virtual void nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) = 0;
  virtual void nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) = 0;

  // Calls.
  virtual void call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;
  virtual void call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;
  virtual void call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) = 0;
  virtual void call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) = 0;

  // Typed access to the PluginRegistry for datastores.
  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Datastore, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), plugins::kinds::datastore, std::move(scheme));
  }

  static std::shared_ptr<Datastore> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<Datastore>(std::move(logger), plugins::kinds::datastore, url_string);
  }
};

}  // namespace athenasip::datastores
