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
#include "../types/url.h"
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

  std::string get_driver_name() const override;

  bool connect() override;
  void close() override;
  bool is_connected() const override;

  std::shared_ptr<types::Realm> realm_get_by_name(const std::string& realm_name) override;
  bool realm_create(std::shared_ptr<types::Realm> realm) override;
  bool realm_update(std::shared_ptr<types::Realm> realm) override;
  bool realm_delete(const std::string& realm_name) override;
  std::vector<std::shared_ptr<types::Realm>> realm_list() override;

  std::shared_ptr<types::Subscriber> subscriber_get(std::shared_ptr<types::SIPIdentity> identity) override;
  bool subscriber_create(std::shared_ptr<types::Subscriber> subscriber) override;
  bool subscriber_update(std::shared_ptr<types::Subscriber> subscriber) override;
  bool subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) override;
  std::vector<std::shared_ptr<types::Subscriber>> subscriber_list(const std::string& realm_name) override;

  bool subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact, std::uint32_t expires_seconds,
                           const std::string& path) override;
  bool subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  std::vector<types::Location> location_list(std::uint64_t subscriber_id) override;

  bool nonce_create(const std::string& nonce, const std::time_t& expires_at) override;
  bool nonce_check(std::string nonce) override;

  bool call_create(std::shared_ptr<Call>) override;
  bool call_update(std::shared_ptr<Call>) override;
  std::shared_ptr<Call> call_get(const std::string& id) override;
  std::vector<std::shared_ptr<Call>> call_list() override;

 private:
  using BoolCallback = std::function<void(RedisError error, bool value)>;
  using StringCallback = std::function<void(RedisError error, std::optional<std::string> value)>;
  using IntegerCallback = std::function<void(RedisError error, std::int64_t value)>;
  using StringsCallback = std::function<void(RedisError error, std::vector<std::string> value)>;

  void _apply_url(std::shared_ptr<types::URL> url);
  boost::redis::config _make_config() const;

  bool _ping();
  bool _set(std::string key, std::string value);
  bool _set_ex(std::string key, std::string value, std::chrono::seconds expiry);
  std::optional<std::string> _get(std::string key);
  std::int64_t _del(std::string key);
  bool _exists(std::string key);

  // Sets are used as indexes, so listing never needs KEYS or SCAN. KEYS blocks the
  // server, and location_list is on the call path for target determination.
  bool _sadd(std::string key, std::string member);
  bool _srem(std::string key, std::string member);
  std::vector<std::string> _smembers(std::string key);

  void _async_ping(BoolCallback callback);
  void _async_set(std::string key, std::string value, BoolCallback callback);
  void _async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback);
  void _async_get(std::string key, StringCallback callback);
  void _async_del(std::string key, IntegerCallback callback);
  void _async_exists(std::string key, BoolCallback callback);
  void _async_strings(std::string operation, boost::redis::request request, StringsCallback callback);

  void _async_ok(std::string operation, boost::redis::request request, BoolCallback callback);
  void _async_string(std::string operation, boost::redis::request request, StringCallback callback);
  void _async_integer(std::string operation, boost::redis::request request, IntegerCallback callback);

  bool _wait_bool(std::function<void(BoolCallback)> starter);
  std::optional<std::string> _wait_string(std::function<void(StringCallback)> starter);
  std::int64_t _wait_integer(std::function<void(IntegerCallback)> starter);
  std::vector<std::string> _wait_strings(std::function<void(StringsCallback)> starter);

  static std::string _realm_key(const std::string& realm_name);
  static std::string _subscriber_key(const std::string& realm_name, const std::string& user);
  static std::string _location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port);
  static std::string _nonce_key(const std::string& nonce);
  static std::string _call_key(const std::string& call_id);

  static std::string _realm_index_key();
  static std::string _subscriber_index_key(const std::string& realm_name);
  static std::string _location_index_key(std::uint64_t subscriber_id);
  static std::string _call_index_key();

  static std::string _serialise_realm(const std::shared_ptr<types::Realm>& realm);
  static std::string _serialise_subscriber(const std::shared_ptr<types::Subscriber>& subscriber);
  static std::string _serialise_call(const std::shared_ptr<Call>& call);

  std::shared_ptr<types::Realm> _parse_realm(const std::string& value) const;
  std::shared_ptr<Call> _parse_call(const std::string& value) const;

  std::string _duration_ms(std::chrono::high_resolution_clock::time_point start) const;
  RedisError _not_connected_error() const;
  RedisError _timeout_error() const;

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

  std::chrono::milliseconds _sync_timeout{std::chrono::seconds(5)};
};

}  // namespace athenasip::datastores
