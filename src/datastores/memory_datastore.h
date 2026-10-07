//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/session.h"
#include "../types/url.h"
#include "../types/user.h"
#include "datastore.h"

namespace athenasip::datastores {

// The zero-config datastore: everything lives in this process and nothing survives a
// restart. A single node without Redis runs on it, and so do the tests.
class MemoryDatastore : public Datastore {
 public:
  MemoryDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~MemoryDatastore() override;

  std::string name() const override;
  bool configure(const YAML::Node& own_root, const Config& system) override;
  std::string version() const override;

  void connect(plugins::Executor on, plugins::StatusHandler handler) override;
  void close() override;
  bool is_connected() const override;

  void realm_get_by_name(plugins::Executor on, std::string realm_name, plugins::Handler<std::shared_ptr<types::Realm>> handler) override;
  void realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) override;
  void realm_update(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) override;
  void realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) override;
  void realm_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Realm>>> handler) override;

  void user_get(plugins::Executor on, std::string username, plugins::Handler<std::shared_ptr<types::User>> handler) override;
  void user_create(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) override;
  void user_update(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) override;
  void user_delete(plugins::Executor on, std::string username, plugins::StatusHandler handler) override;
  void user_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::User>>> handler) override;

  void session_create(plugins::Executor on, types::Session session, plugins::StatusHandler handler) override;
  void session_get(plugins::Executor on, std::string token_hash, plugins::Handler<std::shared_ptr<types::Session>> handler) override;
  void session_delete(plugins::Executor on, std::string token_hash, plugins::StatusHandler handler) override;
  void session_delete_for_user(plugins::Executor on, std::string username, plugins::StatusHandler handler) override;

  void subscriber_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                      plugins::Handler<std::shared_ptr<types::Subscriber>> handler) override;
  void subscriber_create(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) override;
  void subscriber_update(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) override;
  void subscriber_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) override;
  void subscriber_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Subscriber>>> handler) override;

  void subscriber_register(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, types::Location binding, std::uint32_t expires_seconds,
                           plugins::StatusHandler handler) override;
  void subscriber_unregister(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact,
                             plugins::StatusHandler handler) override;
  void location_list(plugins::Executor on, std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) override;

  void nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) override;
  void nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) override;

  void call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) override;
  void call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) override;

 private:
  // The synchronous work behind the public operations, which deliver these results on the
  // caller's executor.
  std::shared_ptr<types::Realm> _realm_get_by_name(const std::string& realm_name);
  bool _realm_create(std::shared_ptr<types::Realm> realm);
  bool _realm_update(std::shared_ptr<types::Realm> realm);
  bool _realm_delete(const std::string& realm_name);
  std::vector<std::shared_ptr<types::Realm>> _realm_list();

  std::shared_ptr<types::User> _user_get(const std::string& username);
  bool _user_create(std::shared_ptr<types::User> user);
  bool _user_update(std::shared_ptr<types::User> user);
  bool _user_delete(const std::string& username);
  std::vector<std::shared_ptr<types::User>> _user_list();

  bool _session_create(types::Session session);
  std::shared_ptr<types::Session> _session_get(const std::string& token_hash);
  bool _session_delete(const std::string& token_hash);
  bool _session_delete_for_user(const std::string& username);

  // Called with _mutex held: every session that names this user, by User::key().
  std::size_t _session_erase_for_user(const std::string& key);

  std::shared_ptr<types::Subscriber> _subscriber_get(std::shared_ptr<types::SIPIdentity> identity);
  bool _subscriber_create(std::shared_ptr<types::Subscriber> subscriber);
  bool _subscriber_update(std::shared_ptr<types::Subscriber> subscriber);
  bool _subscriber_delete(std::shared_ptr<types::SIPIdentity> identity);
  std::vector<std::shared_ptr<types::Subscriber>> _subscriber_list(const std::string& realm_name);

  bool _subscriber_register(const std::shared_ptr<types::Subscriber>& subscriber, types::Location binding, std::uint32_t expires_seconds);
  bool _subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact);
  std::vector<types::Location> _location_list(std::uint64_t subscriber_id);

  bool _nonce_create(const std::string& nonce, const std::time_t& expires_at);
  bool _nonce_check(std::string nonce);

  bool _call_create(std::shared_ptr<Call> call);
  void _call_prune(std::time_t now);
  bool _call_update(std::shared_ptr<Call> call);
  std::shared_ptr<Call> _call_get(const std::string& id);
  std::vector<std::shared_ptr<Call>> _call_list();

  static std::string _subscriber_key(const std::string& realm_name, const std::string& user);
  static std::string _location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port);

  // Called with _mutex held.
  void _prune_expired();

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  mutable std::mutex _mutex;
  bool _connected = false;

  std::unordered_map<std::string, std::shared_ptr<types::Realm>> _realms;
  std::unordered_map<std::string, std::shared_ptr<types::User>> _users;
  std::unordered_map<std::string, types::Session> _sessions;
  std::unordered_map<std::string, std::shared_ptr<types::Subscriber>> _subscribers;
  std::unordered_map<std::string, types::Location> _locations;
  std::unordered_map<std::string, std::time_t> _nonces;
  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;

  // How long the record of an ended call is kept, in seconds; zero for ever.
  std::uint32_t _call_retention = 30 * 24 * 60 * 60;
};

}  // namespace athenasip::datastores
