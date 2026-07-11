//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sqlite_datastore.h"

#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace athenasip::datastores {
namespace {

class SQLiteStatement {
 public:
  SQLiteStatement(sqlite3* db, const std::string& sql) : _db(db), _sql(sql) {
    if (!_db) {
      throw std::runtime_error("SQLite connection is not open");
    }

    if (sqlite3_prepare_v2(_db, _sql.c_str(), -1, &_stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  SQLiteStatement(const SQLiteStatement&) = delete;
  SQLiteStatement& operator=(const SQLiteStatement&) = delete;

  ~SQLiteStatement() {
    if (_stmt) {
      sqlite3_finalize(_stmt);
      _stmt = nullptr;
    }
  }

  void bind(int index, const std::string& value) {
    if (sqlite3_bind_text(_stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  void bind(int index, const char* value) {
    if (value == nullptr) {
      bind_null(index);
      return;
    }

    if (sqlite3_bind_text(_stmt, index, value, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(_stmt, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  void bind(int index, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
      throw std::runtime_error("SQLite bind uint64 value out of signed int64 range");
    }

    bind(index, static_cast<std::int64_t>(value));
  }

  void bind(int index, std::uint16_t value) { bind(index, static_cast<std::int64_t>(value)); }

  void bind_null(int index) {
    if (sqlite3_bind_null(_stmt, index) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  bool step_row() {
    const int rc = sqlite3_step(_stmt);
    if (rc == SQLITE_ROW) {
      return true;
    }

    if (rc == SQLITE_DONE) {
      return false;
    }

    throw std::runtime_error(sqlite3_errmsg(_db));
  }

  void step_done() {
    const int rc = sqlite3_step(_stmt);
    if (rc != SQLITE_DONE) {
      throw std::runtime_error(sqlite3_errmsg(_db));
    }
  }

  std::string column_string(int index) const {
    if (sqlite3_column_type(_stmt, index) == SQLITE_NULL) {
      throw std::runtime_error("NULL SQLite value cannot be converted to string");
    }

    const auto* value = sqlite3_column_text(_stmt, index);
    return value == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(value));
  }

  std::uint64_t column_uint64(int index) const {
    if (sqlite3_column_type(_stmt, index) == SQLITE_NULL) {
      throw std::runtime_error("NULL SQLite value cannot be converted to uint64");
    }

    const auto value = sqlite3_column_int64(_stmt, index);
    if (value < 0) {
      throw std::runtime_error("Negative SQLite value cannot be converted to uint64");
    }

    return static_cast<std::uint64_t>(value);
  }

  std::uint32_t column_uint32(int index) const {
    const auto value = column_uint64(index);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("SQLite value out of uint32 range");
    }

    return static_cast<std::uint32_t>(value);
  }

  int changes() const { return sqlite3_changes(_db); }

 private:
  sqlite3* _db;
  sqlite3_stmt* _stmt{nullptr};
  std::string _sql;
};

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

SQLiteDatastore::SQLiteDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("sqlite_datastore", std::move(logger))), _url(std::move(url)) {}

SQLiteDatastore::~SQLiteDatastore() { close(); }

std::string SQLiteDatastore::get_driver_name() const { return "AthenaSIP SQLite Driver v0.0.1"; }

bool SQLiteDatastore::connect() {
  if (_db) {
    return true;
  }

  const auto path = _db_path();
  _logger->debug("Opening database: " + path);

  int flags = SQLITE_OPEN_READWRITE;
  int rc = sqlite3_open_v2(path.c_str(), &_db, flags, nullptr);

  if (rc == SQLITE_OK) {
    _logger->info("Opened database: " + path);
    _exec_simple("PRAGMA foreign_keys = ON");
    return true;
  }

  if (_db) {
    sqlite3_close(_db);
    _db = nullptr;
  }

  _logger->debug("Creating new database at: " + path);
  flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  rc = sqlite3_open_v2(path.c_str(), &_db, flags, nullptr);

  if (rc != SQLITE_OK) {
    const std::string error = _db ? sqlite3_errmsg(_db) : "unknown SQLite open error";
    _logger->error("Failed to create database: " + error);
    if (_db) {
      sqlite3_close(_db);
      _db = nullptr;
    }
    return false;
  }

  _logger->info("Created database: " + path);
  _exec_simple("PRAGMA foreign_keys = ON");
  return true;
}

void SQLiteDatastore::close() {
  if (!_db) {
    return;
  }

  sqlite3_close(_db);
  _db = nullptr;
}

bool SQLiteDatastore::is_connected() const { return _db != nullptr; }

std::shared_ptr<types::Realm> SQLiteDatastore::realm_get_by_name(const std::string& realm_name) {
  static const std::string sql =
      "SELECT id, name, nonce_secret, nonce_expiry, registration_timeout "
      "FROM realm WHERE realm.name = ?";

  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("realm_get_by_name: datastore is not connected");
      return nullptr;
    }

    SQLiteStatement stmt(_db, sql);
    stmt.bind(1, realm_name);

    if (!stmt.step_row()) {
      _log_sql(sql, realm_name, 0, start);
      return nullptr;
    }

    auto realm = std::make_shared<types::Realm>(stmt.column_string(1));
    realm->id = stmt.column_uint64(0);
    realm->nonce_secret = stmt.column_string(2);
    realm->nonce_expiry = stmt.column_uint32(3);
    realm->registration_timeout = stmt.column_uint32(4);

    std::size_t rows = 1;
    if (stmt.step_row()) {
      ++rows;
      _logger->warn("Duplicate realm in datastore: " + realm_name + " (at least 2 copies)");
    }

    _log_sql(sql, realm_name, rows, start);
    return realm;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, realm_name, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, realm_name, "Unknown exception");
    return nullptr;
  }
}

std::shared_ptr<types::Subscriber> SQLiteDatastore::subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  static const std::string sql =
      "SELECT subscriber.id, subscriber.name, subscriber.ha1 "
      "FROM subscriber, realm "
      "WHERE subscriber.\"user\" = ? AND realm.name = ? AND subscriber.realm_id = realm.id";

  const std::string params = identity->uri->user + "," + identity->uri->realm;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("subscriber_get: datastore is not connected");
      return nullptr;
    }

    SQLiteStatement stmt(_db, sql);
    stmt.bind(1, identity->uri->user);
    stmt.bind(2, identity->uri->realm);

    if (!stmt.step_row()) {
      _log_sql(sql, params, 0, start);
      return nullptr;
    }

    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = stmt.column_uint64(0);
    subscriber->identity = std::move(identity);
    subscriber->ha1 = stmt.column_string(2);

    std::size_t rows = 1;
    if (stmt.step_row()) {
      ++rows;
      _logger->warn("Duplicate subscriber in datastore: " + subscriber->identity->to_string() + " (at least 2 copies)");
    }

    _log_sql(sql, params, rows, start);
    return subscriber;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return nullptr;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return nullptr;
  }
}

bool SQLiteDatastore::subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string count_sql = "SELECT COUNT(*) FROM location WHERE subscriber_id = ? AND \"user\" = ? AND host = ? AND port = ?";
  static const std::string insert_sql = "INSERT INTO location (subscriber_id, \"user\", host, port, registered_at, nat) VALUES (?,?,?,?,datetime('now'),?)";
  static const std::string update_sql =
      "UPDATE location SET registered_at = datetime('now') WHERE subscriber_id = ? AND \"user\" = ? AND host = ? AND port = ?";

  const auto port = contact->port.value_or(0);
  const std::string params = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);

  try {
    if (!is_connected()) {
      _logger->error("subscriber_register: datastore is not connected");
      return false;
    }

    auto start = std::chrono::high_resolution_clock::now();
    SQLiteStatement count_stmt(_db, count_sql);
    count_stmt.bind(1, subscriber->id);
    count_stmt.bind(2, contact->user);
    count_stmt.bind(3, contact->realm);
    count_stmt.bind(4, static_cast<std::uint16_t>(port));

    if (!count_stmt.step_row()) {
      _logger->error("subscriber_register: COUNT(*) returned no rows");
      return false;
    }

    const auto existing = count_stmt.column_uint64(0);
    _log_sql(count_sql, params, 1, start);

    const std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

    start = std::chrono::high_resolution_clock::now();
    if (existing == 0) {
      SQLiteStatement insert_stmt(_db, insert_sql);
      insert_stmt.bind(1, subscriber->id);
      insert_stmt.bind(2, contact->user);
      insert_stmt.bind(3, contact->realm);
      insert_stmt.bind(4, static_cast<std::uint16_t>(port));
      insert_stmt.bind(5, is_nat);
      insert_stmt.step_done();
      _log_sql(insert_sql, params + "," + is_nat, static_cast<std::size_t>(insert_stmt.changes()), start);
    } else {
      SQLiteStatement update_stmt(_db, update_sql);
      update_stmt.bind(1, subscriber->id);
      update_stmt.bind(2, contact->user);
      update_stmt.bind(3, contact->realm);
      update_stmt.bind(4, static_cast<std::uint16_t>(port));
      update_stmt.step_done();
      _log_sql(update_sql, params, static_cast<std::size_t>(update_stmt.changes()), start);
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

bool SQLiteDatastore::subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  static const std::string sql = "DELETE FROM location WHERE subscriber_id = ? AND \"user\" = ? AND host = ? AND port = ?";

  const auto port = contact->port.value_or(0);
  const std::string params = std::to_string(subscriber->id) + "," + contact->user + "," + contact->realm + "," + std::to_string(port);
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("subscriber_unregister: datastore is not connected");
      return false;
    }

    SQLiteStatement stmt(_db, sql);
    stmt.bind(1, subscriber->id);
    stmt.bind(2, contact->user);
    stmt.bind(3, contact->realm);
    stmt.bind(4, static_cast<std::uint16_t>(port));
    stmt.step_done();

    _log_sql(sql, params, static_cast<std::size_t>(stmt.changes()), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return false;
  }
}

bool SQLiteDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  static const std::string sql = "INSERT INTO nonce (id, expires_at) VALUES (?,?)";

  const auto expires_at_string = _to_utc_datetime_string(expires_at);
  const std::string params = nonce + "," + expires_at_string;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("nonce_create: datastore is not connected");
      return false;
    }

    SQLiteStatement stmt(_db, sql);
    stmt.bind(1, nonce);
    stmt.bind(2, expires_at_string);
    stmt.step_done();

    _log_sql(sql, params, static_cast<std::size_t>(stmt.changes()), start);
    return true;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, params, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, params, "Unknown exception");
    return false;
  }
}

bool SQLiteDatastore::nonce_check(std::string nonce) {
  static const std::string sql = "SELECT COUNT(*) FROM nonce WHERE id = ? AND expires_at > datetime('now')";
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!is_connected()) {
      _logger->error("nonce_check: datastore is not connected");
      return false;
    }

    SQLiteStatement stmt(_db, sql);
    stmt.bind(1, nonce);

    if (!stmt.step_row()) {
      _logger->error("nonce_check: COUNT(*) returned no rows");
      return false;
    }

    const auto count = stmt.column_uint64(0);
    _log_sql(sql, nonce, 1, start);
    return count == 1;
  } catch (const std::exception& ex) {
    _log_sql_error(sql, nonce, ex.what());
    return false;
  } catch (...) {
    _log_sql_error(sql, nonce, "Unknown exception");
    return false;
  }
}

bool SQLiteDatastore::call_create(std::shared_ptr<Call>) {
  return false;
}

std::shared_ptr<Call> SQLiteDatastore::call_get(const std::string& id) {
  return nullptr;
}

std::string SQLiteDatastore::_db_path() const {
  std::string path = _url ? _url->path : std::string{};
  if (!path.empty() && path.front() == '/') {
    path.erase(0, 1);
  }

  return Util::expand_path(path);
}

std::string SQLiteDatastore::_to_utc_datetime_string(const std::time_t& value) const {
  std::tm tm = utc_tm_from_time_t(value);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string SQLiteDatastore::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

void SQLiteDatastore::_log_sql(const std::string& sql, const std::string& params, std::size_t rows,
                               std::chrono::high_resolution_clock::time_point start) const {
  _logger->debug("SQL: " + sql + " [" + Util::trim(params, ",") + "] (" + std::to_string(rows) + " rows, " + _duration_ms(start) + "ms)");
}

void SQLiteDatastore::_log_sql_error(const std::string& sql, const std::string& params, const std::string& error) const {
  _logger->error("SQL: " + sql + " [" + Util::trim(params, ",") + "] Error: " + error);
}

bool SQLiteDatastore::_exec_simple(const std::string& sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(_db, sql.c_str(), nullptr, nullptr, &error);
  if (rc == SQLITE_OK) {
    return true;
  }

  std::string message = error ? error : sqlite3_errmsg(_db);
  sqlite3_free(error);
  _logger->error("SQL: " + sql + " Error: " + message);
  return false;
}

}  // namespace athenasip::datastores
