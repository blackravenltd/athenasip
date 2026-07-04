//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "redis_client.h"

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/redis/src.hpp>
#include <boost/system/system_error.hpp>

#include <future>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <utility>

namespace athenasip::databases {
namespace {

RedisError make_redis_response_error(const std::string& diagnostic) {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::protocol_error), diagnostic);
}

}  // namespace

RedisClient::RedisClient(std::shared_ptr<Logger> logger, std::string host, std::uint16_t port, std::string username, std::string password,
                         std::int32_t database_index, bool use_ssl)
    : _logger(std::make_shared<LoggerScoped>("redis", logger)),
      _host(std::move(host)),
      _port(port),
      _username(std::move(username)),
      _password(std::move(password)),
      _database_index(database_index),
      _use_ssl(use_ssl) {}

RedisClient::~RedisClient() { close(); }

bool RedisClient::connect() {
  if (_started.load()) {
    return ping();
  }

  _logger->debug("URL: " + _host + ":" + std::to_string(_port));

  try {
    _work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(_io_context.get_executor());
    _connection = std::make_shared<boost::redis::connection>(_io_context.get_executor());

    auto cfg = _make_config();
    _connection->async_run(cfg, boost::asio::consign(boost::asio::detached, _connection));

    _io_thread = std::thread([this]() {
      try {
        _io_context.run();
      } catch (const std::exception& ex) {
        _logger->error(std::string("IO thread exception: ") + ex.what());
      } catch (...) {
        _logger->error("IO thread unknown exception.");
      }
    });

    _started.store(true);

    // Check the connection path works. Boost.Redis will queue this until the
    // connection is established, subject to this client's sync timeout.
    if (!ping()) {
      _logger->error("Initial PING failed.");
      close();
      return false;
    }

    return true;
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    close();
    return false;
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    close();
    return false;
  }
}

void RedisClient::close() {
  if (!_started.exchange(false)) {
    return;
  }

  if (_connection) {
    _connection->cancel();
  }

  _work_guard.reset();
  _io_context.stop();

  if (_io_thread.joinable()) {
    _io_thread.join();
  }

  _connection.reset();
  _io_context.restart();
}

bool RedisClient::is_started() const { return _started.load(); }

bool RedisClient::ping() {
  return _wait_bool([this](BoolCallback callback) { async_ping(std::move(callback)); });
}

void RedisClient::async_ping(BoolCallback callback) {
  boost::redis::request req;
  req.push("PING");
  _async_ok("PING", std::move(req), std::move(callback));
}

bool RedisClient::set(std::string key, std::string value) {
  return _wait_bool([this, key = std::move(key), value = std::move(value)](BoolCallback callback) mutable {
    async_set(std::move(key), std::move(value), std::move(callback));
  });
}

bool RedisClient::set_ex(std::string key, std::string value, std::chrono::seconds expiry) {
  return _wait_bool([this, key = std::move(key), value = std::move(value), expiry](BoolCallback callback) mutable {
    async_set_ex(std::move(key), std::move(value), expiry, std::move(callback));
  });
}

bool RedisClient::set_px(std::string key, std::string value, std::chrono::milliseconds expiry) {
  return _wait_bool([this, key = std::move(key), value = std::move(value), expiry](BoolCallback callback) mutable {
    async_set_px(std::move(key), std::move(value), expiry, std::move(callback));
  });
}

std::optional<std::string> RedisClient::get(std::string key) {
  return _wait_string([this, key = std::move(key)](StringCallback callback) mutable { async_get(std::move(key), std::move(callback)); });
}

std::int64_t RedisClient::del(std::string key) {
  return _wait_integer([this, key = std::move(key)](IntegerCallback callback) mutable { async_del(std::move(key), std::move(callback)); });
}

bool RedisClient::exists(std::string key) {
  return _wait_bool([this, key = std::move(key)](BoolCallback callback) mutable { async_exists(std::move(key), std::move(callback)); });
}

bool RedisClient::expire(std::string key, std::chrono::seconds expiry) {
  return _wait_bool([this, key = std::move(key), expiry](BoolCallback callback) mutable { async_expire(std::move(key), expiry, std::move(callback)); });
}

std::int64_t RedisClient::ttl(std::string key) {
  return _wait_integer([this, key = std::move(key)](IntegerCallback callback) mutable { async_ttl(std::move(key), std::move(callback)); });
}

void RedisClient::async_set(std::string key, std::string value, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value);
  _async_ok("SET", std::move(req), std::move(callback));
}

void RedisClient::async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value, "EX", expiry.count());
  _async_ok("SET EX", std::move(req), std::move(callback));
}

void RedisClient::async_set_px(std::string key, std::string value, std::chrono::milliseconds expiry, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value, "PX", expiry.count());
  _async_ok("SET PX", std::move(req), std::move(callback));
}

void RedisClient::async_get(std::string key, StringCallback callback) {
  boost::redis::request req;
  req.push("GET", key);
  _async_string("GET", std::move(req), std::move(callback));
}

void RedisClient::async_del(std::string key, IntegerCallback callback) {
  boost::redis::request req;
  req.push("DEL", key);
  _async_integer("DEL", std::move(req), std::move(callback));
}

void RedisClient::async_exists(std::string key, BoolCallback callback) {
  boost::redis::request req;
  req.push("EXISTS", key);

  _async_integer("EXISTS", std::move(req), [callback = std::move(callback)](RedisError error, std::int64_t value) mutable {
    callback(std::move(error), value > 0);
  });
}

void RedisClient::async_expire(std::string key, std::chrono::seconds expiry, BoolCallback callback) {
  boost::redis::request req;
  req.push("EXPIRE", key, expiry.count());

  _async_integer("EXPIRE", std::move(req), [callback = std::move(callback)](RedisError error, std::int64_t value) mutable {
    callback(std::move(error), value == 1);
  });
}

void RedisClient::async_ttl(std::string key, IntegerCallback callback) {
  boost::redis::request req;
  req.push("TTL", key);
  _async_integer("TTL", std::move(req), std::move(callback));
}

bool RedisClient::json_set(std::string key, std::string json, std::string path) {
  return _wait_bool([this, key = std::move(key), json = std::move(json), path = std::move(path)](BoolCallback callback) mutable {
    async_json_set(std::move(key), std::move(json), std::move(path), std::move(callback));
  });
}

std::optional<std::string> RedisClient::json_get(std::string key, std::string path) {
  return _wait_string([this, key = std::move(key), path = std::move(path)](StringCallback callback) mutable {
    async_json_get(std::move(key), std::move(path), std::move(callback));
  });
}

std::int64_t RedisClient::json_del(std::string key, std::string path) {
  return _wait_integer([this, key = std::move(key), path = std::move(path)](IntegerCallback callback) mutable {
    async_json_del(std::move(key), std::move(path), std::move(callback));
  });
}

void RedisClient::async_json_set(std::string key, std::string json, std::string path, BoolCallback callback) {
  boost::redis::request req;
  req.push("JSON.SET", key, path, json);
  _async_ok("JSON.SET", std::move(req), std::move(callback));
}

void RedisClient::async_json_get(std::string key, std::string path, StringCallback callback) {
  boost::redis::request req;
  req.push("JSON.GET", key, path);
  _async_string("JSON.GET", std::move(req), std::move(callback));
}

void RedisClient::async_json_del(std::string key, std::string path, IntegerCallback callback) {
  boost::redis::request req;
  req.push("JSON.DEL", key, path);
  _async_integer("JSON.DEL", std::move(req), std::move(callback));
}

bool RedisClient::ft_create_json_index(std::string index_name, std::string key_prefix, std::vector<RedisSearchField> fields) {
  return _wait_bool([this, index_name = std::move(index_name), key_prefix = std::move(key_prefix), fields = std::move(fields)](BoolCallback callback) mutable {
    async_ft_create_json_index(std::move(index_name), std::move(key_prefix), std::move(fields), std::move(callback));
  });
}

bool RedisClient::ft_drop_index(std::string index_name, bool delete_documents) {
  return _wait_bool([this, index_name = std::move(index_name), delete_documents](BoolCallback callback) mutable {
    async_ft_drop_index(std::move(index_name), delete_documents, std::move(callback));
  });
}

RedisGenericResult RedisClient::ft_search(std::string index_name, std::string query, std::uint64_t offset, std::uint64_t count,
                                          std::vector<std::string> return_fields, std::uint32_t dialect) {
  return _wait_generic([this, index_name = std::move(index_name), query = std::move(query), offset, count, return_fields = std::move(return_fields),
                        dialect](GenericCallback callback) mutable {
    async_ft_search(std::move(index_name), std::move(query), offset, count, std::move(return_fields), dialect, std::move(callback));
  });
}

void RedisClient::async_ft_create_json_index(std::string index_name, std::string key_prefix, std::vector<RedisSearchField> fields, BoolCallback callback) {
  auto args = _make_ft_create_json_index_args(index_name, key_prefix, fields);
  auto req = _make_command("FT.CREATE", args);
  _async_ok("FT.CREATE", std::move(req), std::move(callback));
}

void RedisClient::async_ft_drop_index(std::string index_name, bool delete_documents, BoolCallback callback) {
  std::vector<std::string> args{std::move(index_name)};
  if (delete_documents) {
    args.emplace_back("DD");
  }

  auto req = _make_command("FT.DROPINDEX", args);
  _async_ok("FT.DROPINDEX", std::move(req), std::move(callback));
}

void RedisClient::async_ft_search(std::string index_name, std::string query, std::uint64_t offset, std::uint64_t count,
                                  std::vector<std::string> return_fields, std::uint32_t dialect, GenericCallback callback) {
  std::vector<std::string> args{std::move(index_name), std::move(query), "LIMIT", std::to_string(offset), std::to_string(count)};

  if (!return_fields.empty()) {
    args.emplace_back("RETURN");
    args.emplace_back(std::to_string(return_fields.size()));
    for (auto& field : return_fields) {
      args.emplace_back(std::move(field));
    }
  }

  args.emplace_back("DIALECT");
  args.emplace_back(std::to_string(dialect));

  auto req = _make_command("FT.SEARCH", args);
  _async_generic("FT.SEARCH", std::move(req), std::move(callback));
}

RedisGenericResult RedisClient::command(std::string command_name, std::vector<std::string> args) {
  return _wait_generic([this, command_name = std::move(command_name), args = std::move(args)](GenericCallback callback) mutable {
    async_command(std::move(command_name), std::move(args), std::move(callback));
  });
}

void RedisClient::async_command(std::string command_name, std::vector<std::string> args, GenericCallback callback) {
  auto operation = command_name;
  auto req = _make_command(std::move(command_name), args);
  _async_generic(std::move(operation), std::move(req), std::move(callback));
}

void RedisClient::set_sync_timeout(std::chrono::milliseconds timeout) { _sync_timeout = timeout; }

boost::redis::config RedisClient::_make_config() const {
  boost::redis::config cfg;
  cfg.addr.host = _host;
  cfg.addr.port = std::to_string(_port);
  cfg.username = _username;
  cfg.password = _password;
  cfg.database_index = _database_index;
  cfg.use_ssl = _use_ssl;
  return cfg;
}

void RedisClient::_async_ok(std::string operation, boost::redis::request request, BoolCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), false);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::string>>();

  conn->async_exec(*req, *resp, [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec,
                                                                                                                          std::size_t) mutable {
    if (ec) {
      logger->error("Error during: " + operation + ": " + ec.message());
      callback(RedisError(ec), false);
      return;
    }

    auto& result = std::get<0>(*resp);
    if (!result.has_value()) {
      auto error = make_redis_response_error(result.error().diagnostic);
      logger->error("Error during: " + operation + ": " + error.message());
      callback(std::move(error), false);
      return;
    }

    const auto& value = result.value();
    const bool ok = value == "OK" || value == "PONG";

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
    logger->debug(operation + " (" + oss.str() + "ms)");

    callback(RedisError{}, ok);
  });
}

void RedisClient::_async_string(std::string operation, boost::redis::request request, StringCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), std::nullopt);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();

  conn->async_exec(*req, *resp, [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec,
                                                                                                                          std::size_t) mutable {
    if (ec) {
      logger->error("Error during: " + operation + ": " + ec.message());
      callback(RedisError(ec), std::nullopt);
      return;
    }

    auto& result = std::get<0>(*resp);
    if (!result.has_value()) {
      auto error = make_redis_response_error(result.error().diagnostic);
      logger->error("Error during: " + operation + ": " + error.message());
      callback(std::move(error), std::nullopt);
      return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
    logger->debug(operation + " (" + oss.str() + "ms)");

    callback(RedisError{}, result.value());
  });
}

void RedisClient::_async_integer(std::string operation, boost::redis::request request, IntegerCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), 0);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<long long>>();

  conn->async_exec(*req, *resp, [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec,
                                                                                                                          std::size_t) mutable {
    if (ec) {
      logger->error("Error during: " + operation + ": " + ec.message());
      callback(RedisError(ec), 0);
      return;
    }

    auto& result = std::get<0>(*resp);
    if (!result.has_value()) {
      auto error = make_redis_response_error(result.error().diagnostic);
      logger->error("Error during: " + operation + ": " + error.message());
      callback(std::move(error), 0);
      return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
    logger->debug(operation + " (" + oss.str() + "ms)");

    callback(RedisError{}, static_cast<std::int64_t>(result.value()));
  });
}

void RedisClient::_async_generic(std::string operation, boost::redis::request request, GenericCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), boost::redis::generic_response{});
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::generic_response>();

  conn->async_exec(*req, *resp, [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec,
                                                                                                                          std::size_t) mutable {
    if (ec) {
      logger->error("Error during: " + operation + ": " + ec.message());
      callback(RedisError(ec), boost::redis::generic_response{});
      return;
    }

    if (!resp->has_value()) {
      auto error = make_redis_response_error(resp->error().diagnostic);
      logger->error("Error during: " + operation + ": " + error.message());
      callback(std::move(error), std::move(*resp));
      return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
    logger->debug(operation + " (" + oss.str() + "ms)");

    callback(RedisError{}, std::move(*resp));
  });
}

bool RedisClient::_wait_bool(std::function<void(BoolCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, bool>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, bool value) mutable { promise->set_value({std::move(error), value}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return false;
  }

  auto [error, value] = future.get();
  return !error && value;
}

std::optional<std::string> RedisClient::_wait_string(std::function<void(StringCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, std::optional<std::string>>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, std::optional<std::string> value) mutable { promise->set_value({std::move(error), std::move(value)}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return std::nullopt;
  }

  auto [error, value] = future.get();
  return error ? std::nullopt : value;
}

std::int64_t RedisClient::_wait_integer(std::function<void(IntegerCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, std::int64_t>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, std::int64_t value) mutable { promise->set_value({std::move(error), value}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return 0;
  }

  auto [error, value] = future.get();
  return error ? 0 : value;
}

RedisGenericResult RedisClient::_wait_generic(std::function<void(GenericCallback)> starter) {
  auto promise = std::make_shared<std::promise<RedisGenericResult>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, boost::redis::generic_response response) mutable {
    promise->set_value(RedisGenericResult{std::move(error), std::move(response)});
  });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return RedisGenericResult{std::move(error), boost::redis::generic_response{}};
  }

  return future.get();
}

boost::redis::request RedisClient::_make_command(std::string command_name, const std::vector<std::string>& args) const {
  boost::redis::request req;

  if (args.empty()) {
    req.push(command_name);
  } else {
    req.push_range(command_name, args);
  }

  return req;
}

std::vector<std::string> RedisClient::_make_ft_create_json_index_args(const std::string& index_name, const std::string& key_prefix,
                                                                      const std::vector<RedisSearchField>& fields) const {
  std::vector<std::string> args{index_name, "ON", "JSON", "PREFIX", "1", key_prefix, "SCHEMA"};

  for (const auto& field : fields) {
    args.emplace_back(field.identifier);

    if (!field.alias.empty()) {
      args.emplace_back("AS");
      args.emplace_back(field.alias);
    }

    args.emplace_back(_field_type_to_string(field.type));

    if (field.sortable) {
      args.emplace_back("SORTABLE");
    }

    if (field.no_index) {
      args.emplace_back("NOINDEX");
    }

    for (const auto& extra_arg : field.extra_args) {
      args.emplace_back(extra_arg);
    }
  }

  return args;
}

std::string RedisClient::_field_type_to_string(RedisSearchFieldType type) const {
  switch (type) {
    case RedisSearchFieldType::Text:
      return "TEXT";
    case RedisSearchFieldType::Tag:
      return "TAG";
    case RedisSearchFieldType::Numeric:
      return "NUMERIC";
    case RedisSearchFieldType::Geo:
      return "GEO";
    case RedisSearchFieldType::GeoShape:
      return "GEOSHAPE";
    case RedisSearchFieldType::Vector:
      return "VECTOR";
  }

  return "TEXT";
}

std::string RedisClient::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

RedisError RedisClient::_not_connected_error() const {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::not_connected), "Redis connection is not started");
}

RedisError RedisClient::_timeout_error() const {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::timed_out), "Redis synchronous command timed out");
}

}  // namespace athenasip::databases
