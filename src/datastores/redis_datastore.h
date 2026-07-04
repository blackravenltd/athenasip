//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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

  bool connect() override;
  void close() override;
  bool is_connected() const override;

  std::shared_ptr<types::Realm> realm_get_by_name(const std::string& realm_name) override;
  std::shared_ptr<types::Subscriber> subscriber_get(std::shared_ptr<types::SIPIdentity> identity) override;
  bool subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  bool subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  bool nonce_create(const std::string& nonce, const std::time_t& expires_at) override;
  bool nonce_check(std::string nonce) override;

 private:
  using BoolCallback = std::function<void(RedisError error, bool value)>;
  using StringCallback = std::function<void(RedisError error, std::optional<std::string> value)>;
  using IntegerCallback = std::function<void(RedisError error, std::int64_t value)>;

  void _apply_url(std::shared_ptr<types::URL> url);
  boost::redis::config _make_config() const;

  bool _ping();
  bool _set(std::string key, std::string value);
  bool _set_ex(std::string key, std::string value, std::chrono::seconds expiry);
  std::optional<std::string> _get(std::string key);
  std::int64_t _del(std::string key);
  bool _exists(std::string key);

  void _async_ping(BoolCallback callback);
  void _async_set(std::string key, std::string value, BoolCallback callback);
  void _async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback);
  void _async_get(std::string key, StringCallback callback);
  void _async_del(std::string key, IntegerCallback callback);
  void _async_exists(std::string key, BoolCallback callback);

  void _async_ok(std::string operation, boost::redis::request request, BoolCallback callback);
  void _async_string(std::string operation, boost::redis::request request, StringCallback callback);
  void _async_integer(std::string operation, boost::redis::request request, IntegerCallback callback);

  bool _wait_bool(std::function<void(BoolCallback)> starter);
  std::optional<std::string> _wait_string(std::function<void(StringCallback)> starter);
  std::int64_t _wait_integer(std::function<void(IntegerCallback)> starter);

  static std::string _realm_key(const std::string& realm_name);
  static std::string _subscriber_key(const std::string& realm_name, const std::string& user);
  static std::string _location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port);
  static std::string _nonce_key(const std::string& nonce);

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
