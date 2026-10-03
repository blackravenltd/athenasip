//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/session.h"
#include "../types/url.h"
#include "../types/user.h"
#include "../util.h"
#include "datastore.h"

namespace athenasip::datastores {

class RedisError {
 public:
  RedisError() = default;
  explicit RedisError(boost::system::error_code ec) : ec(ec) {}
  RedisError(boost::system::error_code ec, std::string diagnostic) : ec(ec), diagnostic(std::move(diagnostic)) {}

  bool ok() const { return !ec && diagnostic.empty(); }
  explicit operator bool() const { return !ok(); }

  std::string message() const {
    if (!diagnostic.empty()) {
      return diagnostic;
    }

    return ec ? ec.message() : "";
  }

  boost::system::error_code ec;
  std::string diagnostic;
};

class RedisDatastore : public Datastore {
 public:
  RedisDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~RedisDatastore() override;

  std::string name() const override;
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

  void account_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::Handler<std::shared_ptr<types::Account>> handler) override;
  void account_create(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) override;
  void account_update(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) override;
  void account_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) override;
  void account_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Account>>> handler) override;

  void account_register(plugins::Executor on, std::shared_ptr<types::Account> account, types::Location binding, std::uint32_t expires_seconds,
                        plugins::StatusHandler handler) override;
  void account_unregister(plugins::Executor on, std::shared_ptr<types::Account> account, std::shared_ptr<types::SIPUri> contact,
                          plugins::StatusHandler handler) override;
  void location_list(plugins::Executor on, std::uint64_t account_id, plugins::Handler<std::vector<types::Location>> handler) override;

  void nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) override;
  void nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) override;

  void call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) override;
  void call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) override;

 private:
  using BoolCallback = std::function<void(RedisError error, bool value)>;
  using StringCallback = std::function<void(RedisError error, std::optional<std::string> value)>;
  using IntegerCallback = std::function<void(RedisError error, std::int64_t value)>;
  using StringsCallback = std::function<void(RedisError error, std::vector<std::string> value)>;

  void _apply_url(std::shared_ptr<types::URL> url);
  void _realm_delete_record(plugins::Executor on, plugins::StatusHandler handler, std::string realm_name);
  boost::redis::config _make_config() const;

  void _async_ping(BoolCallback callback);
  void _async_set(std::string key, std::string value, BoolCallback callback);
  void _async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback);
  void _async_get(std::string key, StringCallback callback);
  void _async_del(std::string key, IntegerCallback callback);
  void _async_exists(std::string key, BoolCallback callback);

  // Sets are used as indexes, so listing never needs KEYS or SCAN. KEYS blocks the
  // server, and location_list is on the call path for target determination.
  void _async_sadd(std::string key, std::string member, BoolCallback callback);
  void _async_srem(std::string key, std::string member, BoolCallback callback);
  void _async_smembers(std::string key, StringsCallback callback);
  void _async_strings(std::string operation, boost::redis::request request, StringsCallback callback);

  void _async_ok(std::string operation, boost::redis::request request, BoolCallback callback);
  void _async_string(std::string operation, boost::redis::request request, StringCallback callback);
  void _async_integer(std::string operation, boost::redis::request request, IntegerCallback callback);

  static std::string _realm_key(const std::string& realm_name);
  static std::string _account_key(const std::string& realm_name, const std::string& user);
  static std::string _location_key(std::uint64_t account_id, const std::string& user, const std::string& host, std::uint16_t port);
  static std::string _nonce_key(const std::string& nonce);
  static std::string _user_key(const std::string& username);
  static std::string _session_key(const std::string& token_hash);
  static std::string _call_key(const std::string& call_id);

  static std::string _realm_index_key();
  static std::string _account_index_key(const std::string& realm_name);
  static std::string _location_index_key(std::uint64_t account_id);
  static std::string _call_index_key();
  static std::string _user_index_key();

  // Every session a user holds, so revoking by username never needs KEYS. A session that
  // reaches its own expiry leaves its hash behind here; the listing side removes it.
  static std::string _session_index_key(const std::string& username);

  static std::string _serialise_realm(const std::shared_ptr<types::Realm>& realm);
  static std::string _serialise_user(const std::shared_ptr<types::User>& user);
  static std::string _serialise_session(const types::Session& session);
  static std::string _serialise_account(const std::shared_ptr<types::Account>& account);
  static std::string _serialise_call(const std::shared_ptr<Call>& call);

  std::shared_ptr<types::Realm> _parse_realm(const std::string& value) const;
  std::shared_ptr<types::User> _parse_user(const std::string& value) const;
  std::shared_ptr<types::Session> _parse_session(const std::string& value) const;

  // Both deletes need this, and it must not go through the public operation: user_delete
  // has already answered whether there was a user to delete.
  void _session_delete_all_for(std::string key, std::function<void(RedisError)> done);
  std::shared_ptr<Call> _parse_call(const std::string& value) const;

  std::string _duration_ms(std::chrono::high_resolution_clock::time_point start) const;
  RedisError _not_connected_error() const;

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::string _host{"127.0.0.1"};
  std::uint16_t _port{6379};
  std::string _username{"default"};
  std::string _password;
  std::int32_t _database_index{0};
  bool _use_ssl{false};

  boost::asio::io_context _io_context;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work_guard;
  std::shared_ptr<boost::redis::connection> _connection;
  std::thread _io_thread;
  std::atomic_bool _started{false};
};

}  // namespace athenasip::datastores
