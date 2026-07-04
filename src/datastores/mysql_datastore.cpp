//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mysql_datastore.h"

#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace athenasip::datastores {

namespace {

std::uint64_t mysql_uint64(const mysqlx::Value& value) {
  if (value.getType() == mysqlx::Value::Type::UINT64) {
    return value.get<std::uint64_t>();
  }

  if (value.getType() == mysqlx::Value::Type::INT64) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      throw std::runtime_error("Negative MySQL value cannot be converted to uint64");
    }
    return static_cast<std::uint64_t>(signed_value);
  }

  return value.get<std::uint64_t>();
}

std::uint32_t mysql_uint32(const mysqlx::Value& value) {
  const auto converted = mysql_uint64(value);
  if (converted > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("MySQL value out of uint32 range");
  }

  return static_cast<std::uint32_t>(converted);
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

MySQLDatastore::MySQLDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("mysql_datastore", std::move(logger))), _url(std::move(url)) {}

MySQLDatastore::~MySQLDatastore() { close(); }

bool MySQLDatastore::connect() {
  if (_session) {
    return true;
  }

  try {
    // MySQL Connector/C++ X DevAPI expects mysqlx:// style URLs.
    _url->scheme = "mysqlx";
    _logger->debug("URL: " + _url->to_string());
    _session = std::make_shared<mysqlx::Session>(_url->to_string());
    return true;
  } catch (const mysqlx::Error& ex) {
    _logger->error(std::string("Error while connecting: ") + ex.what());
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
  }

  _session.reset();
  return false;
}

void MySQLDatastore::close() {
  if (!_session) {
    return;
  }

  try {
    _session->close();
  } catch (const std::exception& ex) {
    _logger->warn(std::string("Exception while closing connection: ") + ex.what());
  } catch (...) {
    _logger->warn("Unknown exception while closing connection.");
  }

  _session.reset();
}

bool MySQLDatastore::is_connected() const { return static_cast<bool>(_session); }

std::shared_ptr<types::Realm> MySQLDatastore::realm_get_by_name(const std::string& realm_name) {
  static const std::string sql =
      "SELECT `id`, `name`, `nonce_secret`, `nonce_expiry`, `registration_timeout` "
      "FROM `realm` WHERE `realm`.`name` = ?";

  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_session) {
      _logger->error("realm_get_by_name: datastore is not connected");
      return nullptr;
    }

    auto stmt = _session->sql(sql);
    stmt.bind(realm_name);
    auto result = stmt.execute();
    std::vector<mysqlx::Row> rows = result.fetchAll();

    _log_sql(sql, realm_name, rows.size(), start);

    if (rows.empty()) {
      return nullptr;
    }

    if (rows.size() > 1) {
      _logger->warn("Duplicate realm in datastore: " + realm_name + " (" + std::to_string(rows.size()) + " copies)");
    }

    const auto& row = rows[0];
    auto realm = std::make_shared<types::Realm>(row[1].get<std::string>());
    realm->id = mysql_uint64(row[0]);
    realm->nonce_secret = row[2].get<std::string>();
    realm->nonce_expiry = mysql_uint32(row[3]);
    realm->registration_timeout = mysql_uint32(row[4]);
    return realm;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, realm_name, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, realm_name, "Unknown exception");
    return nullptr;
  }
}

std::shared_ptr<types::Subscriber> MySQLDatastore::subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  static const std::string sql =
      "SELECT `subscriber`.`id`, `subscriber`.`name`, `subscriber`.`ha1` "
      "FROM `subscriber`, `realm` "
      "WHERE `subscriber`.`user` = ? AND `realm`.`name` = ? AND `subscriber`.`realm_id` = `realm`.`id`";

  const std::string params = identity->uri->user + "," + identity->uri->realm;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_session) {
      _logger->error("subscriber_get: datastore is not connected");
      return nullptr;
    }

    auto stmt = _session->sql(sql);
    stmt.bind(identity->uri->user, identity->uri->realm);
    auto result = stmt.execute();
    std::vector<mysqlx::Row> rows = result.fetchAll();

    _log_sql(sql, params, rows.size(), start);

    if (rows.empty()) {
      return nullptr;
    }

    if (rows.size() > 1) {
      _logger->warn("Duplicate subscriber in datastore: " + identity->to_string() + " (" + std::to_string(rows.size()) + " copies)");
    }

    const auto& row = rows[0];
    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = mysql_uint64(row[0]);
    subscriber->identity = std::move(identity);
    subscriber->ha1 = row[2].get<std::string>();
    return subscriber;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return nullptr;
  }
}

bool MySQLDatastore::subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string count_sql = "SELECT COUNT(*) FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?";
  static const std::string insert_sql =
      "INSERT INTO `location` (`subscriber_id`, `user`, `host`, `port`, `registered_at`, `nat`) VALUES (?,?,?,?,UTC_TIMESTAMP(),?)";
  static const std::string update_sql =
      "UPDATE `location` SET `registered_at` = UTC_TIMESTAMP() WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?";

  const auto port = contact->port.value_or(0);
  const std::string params = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);

  try {
    if (!_session) {
      _logger->error("subscriber_register: datastore is not connected");
      return false;
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto count_stmt = _session->sql(count_sql);
    count_stmt.bind(subscriber->id, contact->user, contact->realm, port);
    auto count_result = count_stmt.execute();
    std::vector<mysqlx::Row> count_rows = count_result.fetchAll();
    _log_sql(count_sql, params, count_rows.size(), start);

    if (count_rows.empty()) {
      _logger->error("subscriber_register: COUNT(*) returned no rows");
      return false;
    }

    const auto existing = mysql_uint64(count_rows[0][0]);
    const std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

    start = std::chrono::high_resolution_clock::now();
    if (existing == 0) {
      auto insert_stmt = _session->sql(insert_sql);
      insert_stmt.bind(subscriber->id, contact->user, contact->realm, port, is_nat);
      auto insert_result = insert_stmt.execute();
      _log_sql(insert_sql, params + "," + is_nat, static_cast<std::size_t>(insert_result.getAffectedItemsCount()), start);
    } else {
      auto update_stmt = _session->sql(update_sql);
      update_stmt.bind(subscriber->id, contact->user, contact->realm, port);
      auto update_result = update_stmt.execute();
      _log_sql(update_sql, params, static_cast<std::size_t>(update_result.getAffectedItemsCount()), start);
    }

    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(count_sql, params, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(count_sql, params, "Unknown exception");
    return false;
  }
}

bool MySQLDatastore::subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string sql = "DELETE FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?";

  const auto port = contact->port.value_or(0);
  const std::string params = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_session) {
      _logger->error("subscriber_unregister: datastore is not connected");
      return false;
    }

    auto stmt = _session->sql(sql);
    stmt.bind(subscriber->id, contact->user, contact->realm, port);
    auto result = stmt.execute();
    _log_sql(sql, params, static_cast<std::size_t>(result.getAffectedItemsCount()), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return false;
  }
}

bool MySQLDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  static const std::string sql = "INSERT INTO `nonce` (`id`, `expires_at`) VALUES (?,?)";

  const auto expires_at_string = _to_utc_datetime_string(expires_at);
  const std::string params = nonce + "," + expires_at_string;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_session) {
      _logger->error("nonce_create: datastore is not connected");
      return false;
    }

    auto stmt = _session->sql(sql);
    stmt.bind(nonce, expires_at_string);
    auto result = stmt.execute();
    _log_sql(sql, params, static_cast<std::size_t>(result.getAffectedItemsCount()), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return false;
  }
}

bool MySQLDatastore::nonce_check(std::string nonce) {
  static const std::string sql = "SELECT COUNT(*) FROM `nonce` WHERE `id` = ? AND `expires_at` > UTC_TIMESTAMP()";
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_session) {
      _logger->error("nonce_check: datastore is not connected");
      return false;
    }

    auto stmt = _session->sql(sql);
    stmt.bind(nonce);
    auto result = stmt.execute();
    std::vector<mysqlx::Row> rows = result.fetchAll();
    _log_sql(sql, nonce, rows.size(), start);

    if (rows.empty()) {
      _logger->error("nonce_check: COUNT(*) returned no rows");
      return false;
    }

    return mysql_uint64(rows[0][0]) == 1;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, nonce, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, nonce, "Unknown exception");
    return false;
  }
}

std::string MySQLDatastore::_to_utc_datetime_string(const std::time_t& value) const {
  std::tm tm = utc_tm_from_time_t(value);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string MySQLDatastore::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

void MySQLDatastore::_log_sql(const std::string& sql, const std::string& params, std::size_t rows, std::chrono::high_resolution_clock::time_point start) const {
  _logger->debug("SQL: " + sql + " [" + Util::trim(params, ",") + "] (" + std::to_string(rows) + " rows, " + _duration_ms(start) + "ms)");
}

void MySQLDatastore::_log_sql_error(const std::string& sql, const std::string& params, const std::string& error) const {
  _logger->error("SQL: " + sql + " [" + Util::trim(params, ",") + "] Error: " + error);
}

}  // namespace athenasip::datastores
