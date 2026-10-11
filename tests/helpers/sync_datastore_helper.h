//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "datastores/datastore.h"
#include "global_io_context.h"
#include "plugins/plugin.h"

// A blocking view of a datastore, for tests only. Production callers run on the Core strand and must never wait;
// a test is not on the strand. Tests of the async behaviour itself call the datastore directly.
class SyncDatastore {
 public:
  explicit SyncDatastore(std::shared_ptr<athenasip::datastores::Datastore> store) : _store(std::move(store)) {}

  // Synchronous in the contract.
  std::string name() const { return _store->name(); }
  std::string version() const { return _store->version(); }
  bool is_connected() const { return _store->is_connected(); }
  void close() { _store->close(); }

  std::shared_ptr<athenasip::datastores::Datastore> driver() const { return _store; }

  bool connect() {
    return _status([this](auto on, auto handler) { _store->connect(std::move(on), std::move(handler)); });
  }

  std::shared_ptr<athenasip::types::Realm> realm_get_by_name(const std::string& realm_name) {
    return _value<std::shared_ptr<athenasip::types::Realm>>(
        [this, realm_name](auto on, auto handler) { _store->realm_get_by_name(std::move(on), realm_name, std::move(handler)); });
  }

  bool realm_create(std::shared_ptr<athenasip::types::Realm> realm) {
    return _status([this, realm](auto on, auto handler) { _store->realm_create(std::move(on), realm, std::move(handler)); });
  }

  bool realm_update(std::shared_ptr<athenasip::types::Realm> realm) {
    return _status([this, realm](auto on, auto handler) { _store->realm_update(std::move(on), realm, std::move(handler)); });
  }

  bool realm_delete(const std::string& realm_name) {
    return _status([this, realm_name](auto on, auto handler) { _store->realm_delete(std::move(on), realm_name, std::move(handler)); });
  }

  std::vector<std::shared_ptr<athenasip::types::Realm>> realm_list() {
    return _value<std::vector<std::shared_ptr<athenasip::types::Realm>>>(
        [this](auto on, auto handler) { _store->realm_list(std::move(on), std::move(handler)); });
  }

  std::shared_ptr<athenasip::types::User> user_get(const std::string& username) {
    return _value<std::shared_ptr<athenasip::types::User>>(
        [this, username](auto on, auto handler) { _store->user_get(std::move(on), username, std::move(handler)); });
  }

  bool user_create(std::shared_ptr<athenasip::types::User> user) {
    return _status([this, user](auto on, auto handler) { _store->user_create(std::move(on), user, std::move(handler)); });
  }

  bool user_update(std::shared_ptr<athenasip::types::User> user) {
    return _status([this, user](auto on, auto handler) { _store->user_update(std::move(on), user, std::move(handler)); });
  }

  bool user_delete(const std::string& username) {
    return _status([this, username](auto on, auto handler) { _store->user_delete(std::move(on), username, std::move(handler)); });
  }

  std::vector<std::shared_ptr<athenasip::types::User>> user_list() {
    return _value<std::vector<std::shared_ptr<athenasip::types::User>>>(
        [this](auto on, auto handler) { _store->user_list(std::move(on), std::move(handler)); });
  }

  std::shared_ptr<athenasip::types::Trunk> trunk_get(const std::string& name) {
    return _value<std::shared_ptr<athenasip::types::Trunk>>(
        [this, name](auto on, auto handler) { _store->trunk_get(std::move(on), name, std::move(handler)); });
  }

  bool trunk_create(std::shared_ptr<athenasip::types::Trunk> trunk) {
    return _status([this, trunk](auto on, auto handler) { _store->trunk_create(std::move(on), trunk, std::move(handler)); });
  }

  bool trunk_update(std::shared_ptr<athenasip::types::Trunk> trunk) {
    return _status([this, trunk](auto on, auto handler) { _store->trunk_update(std::move(on), trunk, std::move(handler)); });
  }

  bool trunk_delete(const std::string& name) {
    return _status([this, name](auto on, auto handler) { _store->trunk_delete(std::move(on), name, std::move(handler)); });
  }

  std::vector<std::shared_ptr<athenasip::types::Trunk>> trunk_list() {
    return _value<std::vector<std::shared_ptr<athenasip::types::Trunk>>>(
        [this](auto on, auto handler) { _store->trunk_list(std::move(on), std::move(handler)); });
  }

  bool lease(const std::string& name, const std::string& holder, std::uint32_t seconds) {
    return _value<bool>([this, name, holder, seconds](auto on, auto handler) { _store->lease(std::move(on), name, holder, seconds, std::move(handler)); });
  }

  std::int64_t counter_add(const std::string& name, std::int64_t delta, std::uint32_t seconds = 0) {
    return _value<std::int64_t>(
        [this, name, delta, seconds](auto on, auto handler) { _store->counter_add(std::move(on), name, delta, seconds, std::move(handler)); });
  }

  std::int64_t counter_get(const std::string& name) {
    return _value<std::int64_t>([this, name](auto on, auto handler) { _store->counter_get(std::move(on), name, std::move(handler)); });
  }

  bool session_create(athenasip::types::Session session) {
    return _status([this, session](auto on, auto handler) { _store->session_create(std::move(on), session, std::move(handler)); });
  }

  std::shared_ptr<athenasip::types::Session> session_get(const std::string& token_hash) {
    return _value<std::shared_ptr<athenasip::types::Session>>(
        [this, token_hash](auto on, auto handler) { _store->session_get(std::move(on), token_hash, std::move(handler)); });
  }

  bool session_delete(const std::string& token_hash) {
    return _status([this, token_hash](auto on, auto handler) { _store->session_delete(std::move(on), token_hash, std::move(handler)); });
  }

  bool session_delete_for_user(const std::string& username) {
    return _status([this, username](auto on, auto handler) { _store->session_delete_for_user(std::move(on), username, std::move(handler)); });
  }

  std::shared_ptr<athenasip::types::Subscriber> subscriber_get(std::shared_ptr<athenasip::types::SIPIdentity> identity) {
    return _value<std::shared_ptr<athenasip::types::Subscriber>>(
        [this, identity](auto on, auto handler) { _store->subscriber_get(std::move(on), identity, std::move(handler)); });
  }

  bool subscriber_create(std::shared_ptr<athenasip::types::Subscriber> subscriber) {
    return _status([this, subscriber](auto on, auto handler) { _store->subscriber_create(std::move(on), subscriber, std::move(handler)); });
  }

  bool subscriber_update(std::shared_ptr<athenasip::types::Subscriber> subscriber) {
    return _status([this, subscriber](auto on, auto handler) { _store->subscriber_update(std::move(on), subscriber, std::move(handler)); });
  }

  bool subscriber_delete(std::shared_ptr<athenasip::types::SIPIdentity> identity) {
    return _status([this, identity](auto on, auto handler) { _store->subscriber_delete(std::move(on), identity, std::move(handler)); });
  }

  std::vector<std::shared_ptr<athenasip::types::Subscriber>> subscriber_list(const std::string& realm_name) {
    return _value<std::vector<std::shared_ptr<athenasip::types::Subscriber>>>(
        [this, realm_name](auto on, auto handler) { _store->subscriber_list(std::move(on), realm_name, std::move(handler)); });
  }

  bool subscriber_register(std::shared_ptr<athenasip::types::Subscriber> subscriber, athenasip::types::Location binding, std::uint32_t expires_seconds) {
    return _status([this, subscriber, binding, expires_seconds](auto on, auto handler) {
      _store->subscriber_register(std::move(on), subscriber, binding, expires_seconds, std::move(handler));
    });
  }

  // The single-node case: a contact and a path, with no flow and no node holding one.
  bool subscriber_register(std::shared_ptr<athenasip::types::Subscriber> subscriber, std::shared_ptr<athenasip::types::SIPUri> contact,
                           std::uint32_t expires_seconds, const std::string& path) {
    athenasip::types::Location binding;
    binding.contact = std::move(contact);
    binding.path = path;

    return subscriber_register(std::move(subscriber), std::move(binding), expires_seconds);
  }

  bool subscriber_unregister(std::shared_ptr<athenasip::types::Subscriber> subscriber, std::shared_ptr<athenasip::types::SIPUri> contact) {
    return _status(
        [this, subscriber, contact](auto on, auto handler) { _store->subscriber_unregister(std::move(on), subscriber, contact, std::move(handler)); });
  }

  std::vector<athenasip::types::Location> location_list(std::uint64_t subscriber_id) {
    return _value<std::vector<athenasip::types::Location>>(
        [this, subscriber_id](auto on, auto handler) { _store->location_list(std::move(on), subscriber_id, std::move(handler)); });
  }

  bool nonce_create(const std::string& nonce, std::time_t expires_at) {
    return _status([this, nonce, expires_at](auto on, auto handler) { _store->nonce_create(std::move(on), nonce, expires_at, std::move(handler)); });
  }

  bool nonce_check(const std::string& nonce) {
    return _value<bool>([this, nonce](auto on, auto handler) { _store->nonce_check(std::move(on), nonce, std::move(handler)); });
  }

  bool call_create(std::shared_ptr<athenasip::Call> call) {
    return _status([this, call](auto on, auto handler) { _store->call_create(std::move(on), call, std::move(handler)); });
  }

  bool call_update(std::shared_ptr<athenasip::Call> call) {
    return _status([this, call](auto on, auto handler) { _store->call_update(std::move(on), call, std::move(handler)); });
  }

  std::shared_ptr<athenasip::Call> call_get(const std::string& id) {
    return _value<std::shared_ptr<athenasip::Call>>([this, id](auto on, auto handler) { _store->call_get(std::move(on), id, std::move(handler)); });
  }

  std::vector<std::shared_ptr<athenasip::Call>> call_list() {
    return _value<std::vector<std::shared_ptr<athenasip::Call>>>([this](auto on, auto handler) { _store->call_list(std::move(on), std::move(handler)); });
  }

  // The failure the last call reported.
  const std::string& last_error() const { return _last_error; }

 private:
  template <typename Start>
  bool _status(Start start) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    start(_executor(), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    const auto status = future.get();
    _last_error = status.error;
    return status.ok;
  }

  template <typename T, typename Start>
  T _value(Start start) {
    std::promise<athenasip::plugins::Result<T>> promise;
    auto future = promise.get_future();

    start(_executor(), [&promise](athenasip::plugins::Result<T> result) { promise.set_value(std::move(result)); });

    auto result = future.get();
    _last_error = result.error;
    return std::move(result.value);
  }

  static athenasip::plugins::Executor _executor() { return athenasip::detail::get_global_io_context().get_executor(); }

  std::shared_ptr<athenasip::datastores::Datastore> _store;
  std::string _last_error;
};
