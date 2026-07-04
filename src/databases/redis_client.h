//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

using namespace athenasip::loggers;

namespace athenasip::databases {

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

enum class RedisSearchFieldType {
  Text,
  Tag,
  Numeric,
  Geo,
  GeoShape,
  Vector,
};

class RedisSearchField {
 public:
  std::string identifier;  // Hash field name, or JSON path such as $.username
  std::string alias;       // RediSearch AS alias, e.g. username
  RedisSearchFieldType type = RedisSearchFieldType::Text;
  bool sortable = false;
  bool no_index = false;
  std::vector<std::string> extra_args;
};

class RedisGenericResult {
 public:
  RedisError error;
  boost::redis::generic_response response;

  bool ok() const { return error.ok(); }
  explicit operator bool() const { return ok(); }
};

class RedisClient {
 public:
  using BoolCallback = std::function<void(RedisError error, bool value)>;
  using StringCallback = std::function<void(RedisError error, std::optional<std::string> value)>;
  using IntegerCallback = std::function<void(RedisError error, std::int64_t value)>;
  using GenericCallback = std::function<void(RedisError error, boost::redis::generic_response response)>;

  RedisClient(std::shared_ptr<Logger> logger, std::string host = "127.0.0.1", std::uint16_t port = 6379, std::string username = "default",
              std::string password = "", std::int32_t database_index = 0, bool use_ssl = false);
  ~RedisClient();

  bool connect();
  void close();

  bool is_started() const;
  bool ping();
  void async_ping(BoolCallback callback);

  bool set(std::string key, std::string value);
  bool set_ex(std::string key, std::string value, std::chrono::seconds expiry);
  bool set_px(std::string key, std::string value, std::chrono::milliseconds expiry);
  std::optional<std::string> get(std::string key);
  std::int64_t del(std::string key);
  bool exists(std::string key);
  bool expire(std::string key, std::chrono::seconds expiry);
  std::int64_t ttl(std::string key);

  void async_set(std::string key, std::string value, BoolCallback callback);
  void async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback);
  void async_set_px(std::string key, std::string value, std::chrono::milliseconds expiry, BoolCallback callback);
  void async_get(std::string key, StringCallback callback);
  void async_del(std::string key, IntegerCallback callback);
  void async_exists(std::string key, BoolCallback callback);
  void async_expire(std::string key, std::chrono::seconds expiry, BoolCallback callback);
  void async_ttl(std::string key, IntegerCallback callback);

  bool json_set(std::string key, std::string json, std::string path = "$");
  std::optional<std::string> json_get(std::string key, std::string path = "$");
  std::int64_t json_del(std::string key, std::string path = "$");

  void async_json_set(std::string key, std::string json, std::string path, BoolCallback callback);
  void async_json_set(std::string key, std::string json, BoolCallback callback) { async_json_set(std::move(key), std::move(json), "$", std::move(callback)); }

  void async_json_get(std::string key, std::string path, StringCallback callback);
  void async_json_get(std::string key, StringCallback callback) { async_json_get(std::move(key), "$", std::move(callback)); }

  void async_json_del(std::string key, std::string path, IntegerCallback callback);
  void async_json_del(std::string key, IntegerCallback callback) { async_json_del(std::move(key), "$", std::move(callback)); }

  bool ft_create_json_index(std::string index_name, std::string key_prefix, std::vector<RedisSearchField> fields);
  bool ft_drop_index(std::string index_name, bool delete_documents = false);
  RedisGenericResult ft_search(std::string index_name, std::string query, std::uint64_t offset = 0, std::uint64_t count = 10,
                               std::vector<std::string> return_fields = {}, std::uint32_t dialect = 2);

  void async_ft_create_json_index(std::string index_name, std::string key_prefix, std::vector<RedisSearchField> fields, BoolCallback callback);
  void async_ft_drop_index(std::string index_name, bool delete_documents, BoolCallback callback);
  void async_ft_search(std::string index_name, std::string query, std::uint64_t offset, std::uint64_t count, std::vector<std::string> return_fields,
                       std::uint32_t dialect, GenericCallback callback);

  RedisGenericResult command(std::string command_name, std::vector<std::string> args = {});
  void async_command(std::string command_name, std::vector<std::string> args, GenericCallback callback);

  void set_sync_timeout(std::chrono::milliseconds timeout);

 private:
  boost::redis::config _make_config() const;

  void _async_ok(std::string operation, boost::redis::request request, BoolCallback callback);
  void _async_string(std::string operation, boost::redis::request request, StringCallback callback);
  void _async_integer(std::string operation, boost::redis::request request, IntegerCallback callback);
  void _async_generic(std::string operation, boost::redis::request request, GenericCallback callback);

  bool _wait_bool(std::function<void(BoolCallback)> starter);
  std::optional<std::string> _wait_string(std::function<void(StringCallback)> starter);
  std::int64_t _wait_integer(std::function<void(IntegerCallback)> starter);
  RedisGenericResult _wait_generic(std::function<void(GenericCallback)> starter);

  boost::redis::request _make_command(std::string command_name, const std::vector<std::string>& args) const;
  std::vector<std::string> _make_ft_create_json_index_args(const std::string& index_name, const std::string& key_prefix,
                                                           const std::vector<RedisSearchField>& fields) const;
  std::string _field_type_to_string(RedisSearchFieldType type) const;
  std::string _duration_ms(std::chrono::high_resolution_clock::time_point start) const;
  RedisError _not_connected_error() const;
  RedisError _timeout_error() const;

  std::shared_ptr<Logger> _logger;

  std::string _host;
  std::uint16_t _port;
  std::string _username;
  std::string _password;
  std::int32_t _database_index;
  bool _use_ssl;

  boost::asio::io_context _io_context;
  std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work_guard;
  std::shared_ptr<boost::redis::connection> _connection;
  std::thread _io_thread;
  std::atomic_bool _started{false};

  std::chrono::milliseconds _sync_timeout{std::chrono::seconds(5)};
};

}  // namespace athenasip::databases
