///
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "sqlite_db.h"

namespace athenasip::databases {

SQLiteDB::SQLiteDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url) : DB(std::make_shared<LoggerScoped>("sqlite", logger)), _url(url), _db(nullptr) {}

SQLiteDB::~SQLiteDB() { close(); }

bool SQLiteDB::connect() {
  std::string dbPath = _url->path;
  dbPath.erase(0, 1);
  dbPath = Util::expand_path(dbPath);

  _logger->debug("Opening SQLite database: " + dbPath);

  // Try to open DB
  int flags = SQLITE_OPEN_READWRITE;
  int rc = sqlite3_open_v2(dbPath.c_str(), &_db, flags, nullptr);

  // If OK, return
  if (rc == SQLITE_OK) {
    _logger->info("Opened SQLite database: " + dbPath);
    return true;
  }

  // TODO: Check DB create flag in config

  // Create DB if allowed
  _logger->debug("Creating new database at: " + dbPath);
  flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  rc = sqlite3_open_v2(dbPath.c_str(), &_db, flags, nullptr);

  if (rc != SQLITE_OK) {
    _logger->error("Failed to create database: " + std::string(sqlite3_errmsg(_db)));
    sqlite3_close(_db);
    _db = nullptr;
    return false;
  }

  _logger->info("Created SQLite database: " + dbPath);

  return true;
}

void SQLiteDB::_bind_parameters(sqlite3_stmt* stmt, const std::vector<std::any>& params) {
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].type() == typeid(std::string)) {
      sqlite3_bind_text(stmt, i + 1, std::any_cast<std::string>(params[i]).c_str(), -1, SQLITE_STATIC);
    } else if (params[i].type() == typeid(int)) {
      sqlite3_bind_int(stmt, i + 1, std::any_cast<int>(params[i]));
    } else if (params[i].type() == typeid(double)) {
      sqlite3_bind_double(stmt, i + 1, std::any_cast<double>(params[i]));
    } else {
      throw std::runtime_error("Unsupported parameter type at index " + std::to_string(i));
    }
  }
}

std::shared_ptr<DBResult> SQLiteDB::query(std::string sql, std::vector<std::any> params) {
  auto res = std::make_shared<DBResult>();
  sqlite3_stmt* stmt;

  if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    res->error = sqlite3_errmsg(_db);
    _logger->error("SQLite error: " + res->error);
    return res;
  }

  try {
    _bind_parameters(stmt, params);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      auto row = std::make_shared<DBRow>();
      int col_count = sqlite3_column_count(stmt);
      for (int i = 0; i < col_count; ++i) {
        std::string col_name = sqlite3_column_name(stmt, i);
        auto val = _map_value(sqlite3_column_value(stmt, i));
        row->values[col_name] = val;
        row->column_values.push_back(val);
      }
      res->rows.push_back(row);
    }
    res->rows_affected = sqlite3_changes(_db);
  } catch (const std::exception& ex) {
    res->error = ex.what();
    _logger->error("Standard Exception: " + res->error);
  }

  sqlite3_finalize(stmt);
  return res;
}

std::shared_ptr<DBValue> SQLiteDB::_map_value(sqlite3_value* val) {
  switch (sqlite3_value_type(val)) {
    case SQLITE_INTEGER:
      return std::make_shared<DBValueImpl<int>>(sqlite3_value_int(val));
    case SQLITE_FLOAT:
      return std::make_shared<DBValueImpl<double>>(sqlite3_value_double(val));
    case SQLITE_TEXT:
      return std::make_shared<DBValueImpl<std::string>>(reinterpret_cast<const char*>(sqlite3_value_text(val)));
    case SQLITE_NULL:
      return std::make_shared<DBValueImpl<std::nullptr_t>>();
    default:
      _logger->error("Unsupported column type");
      return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }
}

void SQLiteDB::close() {
  if (_db) {
    sqlite3_close(_db);
    _db = nullptr;
  }
}

bool SQLiteDB::is_created() { return true; }

// Register with DB Drivers
static bool sqlite_registered = [] {
  DB::register_driver<SQLiteDB>("sqlite");
  return true;
}();

}  // namespace athenasip::databases
