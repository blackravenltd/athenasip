//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "postgres_datastore.h"

#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace athenasip::datastores {
namespace {

template <typename Field>
std::uint64_t pg_uint64(const Field& field) {
  if (field.is_null()) {
    throw std::runtime_error("NULL PostgreSQL value cannot be converted to uint64");
  }

  const std::string text = field.c_str();
  if (!text.empty() && text[0] == '-') {
    throw std::runtime_error("Negative PostgreSQL value cannot be converted to uint64");
  }

  return static_cast<std::uint64_t>(std::stoull(text));
}

template <typename Field>
std::uint32_t pg_uint32(const Field& field) {
  const auto value = pg_uint64(field);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("PostgreSQL value out of uint32 range");
  }

  return static_cast<std::uint32_t>(value);
}

template <typename Field>
std::string pg_string(const Field& field) {
  if (field.is_null()) {
    throw std::runtime_error("NULL PostgreSQL value cannot be converted to string");
  }

  field.template as<std::string>();
}

std::tm utc_tm_from_time_t(const std::time_t& value) {
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &value);
#else
  gmtime_r(&value, &tm);
#endif
  return tm;
}

}  // namespace

PostgreSQLDatastore::PostgreSQLDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("postgres_datastore", std::move(logger))), _url(std::move(url)) {}

PostgreSQLDatastore::~PostgreSQLDatastore() { close(); }

std::string PostgreSQLDatastore::get_driver_name() const {
  return "AthenaSIP PostgreSQL Driver v0.0.1";
}

bool PostgreSQLDatastore::connect() {
  if (_connection && _connection->is_open()) {
    return true;
  }

  try {
    _url->scheme = "postgresql";
    _logger->debug("URL: " + _url->to_string());
    _connection = std::make_shared<pqxx::connection>(_url->to_string());

    if (!_connection->is_open()) {
      _logger->error("Connection failed.");
      _connection.reset();
      return false;
    }

    return true;
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
  }

  _connection.reset();
  return false;
}

void PostgreSQLDatastore::close() {
  if (!_connection) {
    return;
  }

  try {
    if (_connection->is_open()) {
      _connection->close();
    }
  } catch (const std::exception& ex) {
    _logger->warn(std::string("Exception while closing connection: ") + ex.what());
  } catch (...) {
    _logger->warn("Unknown exception while closing connection.");
  }

  _connection.reset();
}

bool PostgreSQLDatastore::is_connected() const { return _connection && _connection->is_open(); }

std::shared_ptr<types::Realm> PostgreSQLDatastore::realm_get_by_name(const std::string& realm_name) {
  static const std::string sql =
      "SELECT id, name, nonce_secret, nonce_expiry, registration_timeout "
      "FROM realm WHERE realm.name = $1";

  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("realm_get_by_name: datastore is not connected");
      return nullptr;
    }

    pqxx::work txn(*_connection);
    pqxx::params params{txn};
    params.append(realm_name);
    auto result = txn.exec(sql, params);
    txn.commit();

    _log_sql(sql, realm_name, result.size(), start);

    if (result.empty()) {
      return nullptr;
    }

    if (result.size() > 1) {
      _logger->warn("Duplicate realm in datastore: " + realm_name + " (" + std::to_string(result.size()) + " copies)");
    }

    const auto& row = result[0];
    auto realm = std::make_shared<types::Realm>(pg_string(row["name"]));
    realm->id = pg_uint64(row["id"]);
    realm->nonce_secret = pg_string(row["nonce_secret"]);
    realm->nonce_expiry = pg_uint32(row["nonce_expiry"]);
    realm->registration_timeout = pg_uint32(row["registration_timeout"]);
    return realm;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, realm_name, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, realm_name, "Unknown exception");
    return nullptr;
  }
}

std::shared_ptr<types::Subscriber> PostgreSQLDatastore::subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  static const std::string sql =
      "SELECT subscriber.id, subscriber.name, subscriber.ha1 "
      "FROM subscriber, realm "
      "WHERE subscriber.\"user\" = $1 AND realm.name = $2 AND subscriber.realm_id = realm.id";

  const std::string params_string = identity->uri->user + "," + identity->uri->realm;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("subscriber_get: datastore is not connected");
      return nullptr;
    }

    pqxx::work txn(*_connection);
    pqxx::params params{txn};
    params.append(identity->uri->user);
    params.append(identity->uri->realm);
    auto result = txn.exec(sql, params);
    txn.commit();

    _log_sql(sql, params_string, result.size(), start);

    if (result.empty()) {
      return nullptr;
    }

    if (result.size() > 1) {
      _logger->warn("Duplicate subscriber in datastore: " + identity->to_string() + " (" + std::to_string(result.size()) + " copies)");
    }

    const auto& row = result[0];
    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = pg_uint64(row["id"]);
    subscriber->identity = std::move(identity);
    subscriber->ha1 = pg_string(row["ha1"]);
    return subscriber;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params_string, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, params_string, "Unknown exception");
    return nullptr;
  }
}

bool PostgreSQLDatastore::subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string count_sql = "SELECT COUNT(*) FROM location WHERE subscriber_id = $1 AND \"user\" = $2 AND host = $3 AND port = $4";
  static const std::string insert_sql =
      "INSERT INTO location (subscriber_id, \"user\", host, port, registered_at, nat) "
      "VALUES ($1,$2,$3,$4,(CURRENT_TIMESTAMP AT TIME ZONE 'UTC'),$5)";
  static const std::string update_sql =
      "UPDATE location SET registered_at = (CURRENT_TIMESTAMP AT TIME ZONE 'UTC') "
      "WHERE subscriber_id = $1 AND \"user\" = $2 AND host = $3 AND port = $4";

  const auto port = contact->port.value_or(0);
  const std::string params_string = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);

  try {
    if (!is_connected()) {
      _logger->error("subscriber_register: datastore is not connected");
      return false;
    }

    pqxx::work txn(*_connection);

    auto start = std::chrono::high_resolution_clock::now();
    pqxx::params count_params{txn};
    count_params.append(subscriber->id);
    count_params.append(contact->user);
    count_params.append(contact->realm);
    count_params.append(port);
    auto count_result = txn.exec(count_sql, count_params);
    _log_sql(count_sql, params_string, count_result.size(), start);

    if (count_result.empty()) {
      _logger->error("subscriber_register: COUNT(*) returned no rows");
      return false;
    }

    const auto existing = pg_uint64(count_result[0][0]);
    const std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

    start = std::chrono::high_resolution_clock::now();
    if (existing == 0) {
      pqxx::params insert_params{txn};
      insert_params.append(subscriber->id);
      insert_params.append(contact->user);
      insert_params.append(contact->realm);
      insert_params.append(port);
      insert_params.append(is_nat);
      auto insert_result = txn.exec(insert_sql, insert_params);
      _log_sql(insert_sql, params_string + "," + is_nat, insert_result.affected_rows(), start);
    } else {
      pqxx::params update_params{txn};
      update_params.append(subscriber->id);
      update_params.append(contact->user);
      update_params.append(contact->realm);
      update_params.append(port);
      auto update_result = txn.exec(update_sql, update_params);
      _log_sql(update_sql, params_string, update_result.affected_rows(), start);
    }

    txn.commit();
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(count_sql, params_string, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(count_sql, params_string, "Unknown exception");
    return false;
  }
}

bool PostgreSQLDatastore::subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string sql = "DELETE FROM location WHERE subscriber_id = $1 AND \"user\" = $2 AND host = $3 AND port = $4";

  const auto port = contact->port.value_or(0);
  const std::string params_string = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("subscriber_unregister: datastore is not connected");
      return false;
    }

    pqxx::work txn(*_connection);
    pqxx::params params{txn};
    params.append(subscriber->id);
    params.append(contact->user);
    params.append(contact->realm);
    params.append(port);
    auto result = txn.exec(sql, params);
    txn.commit();

    _log_sql(sql, params_string, result.affected_rows(), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params_string, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params_string, "Unknown exception");
    return false;
  }
}

bool PostgreSQLDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  static const std::string sql = "INSERT INTO nonce (id, expires_at) VALUES ($1,$2)";

  const auto expires_at_string = _to_utc_datetime_string(expires_at);
  const std::string params_string = nonce + "," + expires_at_string;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("nonce_create: datastore is not connected");
      return false;
    }

    pqxx::work txn(*_connection);
    pqxx::params params{txn};
    params.append(nonce);
    params.append(expires_at_string);
    auto result = txn.exec(sql, params);
    txn.commit();

    _log_sql(sql, params_string, result.affected_rows(), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params_string, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params_string, "Unknown exception");
    return false;
  }
}

bool PostgreSQLDatastore::nonce_check(std::string nonce) {
  static const std::string sql = "SELECT COUNT(*) FROM nonce WHERE id = $1 AND expires_at > (CURRENT_TIMESTAMP AT TIME ZONE 'UTC')";
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("nonce_check: datastore is not connected");
      return false;
    }

    pqxx::work txn(*_connection);
    pqxx::params params{txn};
    params.append(nonce);
    auto result = txn.exec(sql, params);
    txn.commit();

    _log_sql(sql, nonce, result.size(), start);

    if (result.empty()) {
      _logger->error("nonce_check: COUNT(*) returned no rows");
      return false;
    }

    return pg_uint64(result[0][0]) == 1;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, nonce, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, nonce, "Unknown exception");
    return false;
  }
}

std::string PostgreSQLDatastore::_to_utc_datetime_string(const std::time_t& value) const {
  std::tm tm = utc_tm_from_time_t(value);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string PostgreSQLDatastore::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

void PostgreSQLDatastore::_log_sql(const std::string& sql, const std::string& params, std::size_t rows, std::chrono::high_resolution_clock::time_point start) const {
  _logger->debug("SQL: " + sql + " [" + Util::trim(params, ",") + "] (" + std::to_string(rows) + " rows, " + _duration_ms(start) + "ms)");
}

void PostgreSQLDatastore::_log_sql_error(const std::string& sql, const std::string& params, const std::string& error) const {
  _logger->error("SQL: " + sql + " [" + Util::trim(params, ",") + "] Error: " + error);
}

}  // namespace athenasip::datastores
