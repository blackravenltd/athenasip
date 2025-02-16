//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "postgres_db.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <stdexcept>
#include <random>

// Helper function to map PostgreSQL type OIDs to type names.
static std::string oid_to_type(pqxx::oid oid) {
  switch (oid) {
    case 21:   return "int2";
    case 23:   return "int4";
    case 20:   return "int8";
    case 700:  return "float4";
    case 701:  return "float8";
    case 1700: return "numeric";
    case 16:   return "bool";
    default:   return "text";
  }
}

namespace athenasip {

namespace {
// Utility to generate a unique prepared statement name.
std::string generate_unique_stmt_name() {
  static std::random_device rd;
  static std::mt19937 mt(rd());
  static std::uniform_int_distribution<int> dist(0, 1000000);
  return "stmt_" + std::to_string(dist(mt));
}
}  // namespace

PostgreSQLDB::PostgreSQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)
    : DB(std::make_shared<LoggerScoped>("postgres", logger)), _url(url) {}

PostgreSQLDB::~PostgreSQLDB() { close(); }

bool PostgreSQLDB::connect() {
  // Adjust scheme if needed (e.g., postgres:// or postgresql://)
  _url->scheme = "postgresql";
  _logger->debug("PostgreSQL URL: " + _url->to_string());
  try {
    _connection = std::make_shared<pqxx::connection>(_url->to_string());
    if (!_connection->is_open()) {
      _logger->error("Connection to PostgreSQL failed.");
      return false;
    }
  } catch (const std::exception &ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    return false;
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    return false;
  }
  return true;
}

std::shared_ptr<DBValue> PostgreSQLDB::_map_value(const pqxx::field &field, const std::string &colType) {
  if (field.is_null()) {
    return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }

  try {
    // Basic mapping based on PostgreSQL type names.
    if (colType == "int2" || colType == "int4" || colType == "int8") {
      return std::make_shared<DBValueImpl<int64_t>>(field.as<int64_t>());
    } else if (colType == "float4" || colType == "float8" || colType == "numeric") {
      return std::make_shared<DBValueImpl<double>>(field.as<double>());
    } else if (colType == "bool") {
      return std::make_shared<DBValueImpl<bool>>(field.as<bool>());
    } else {
      // For character types, text, etc.
      return std::make_shared<DBValueImpl<std::string>>(field.c_str() ? field.c_str() : "");
    }
  } catch (const std::exception &ex) {
    _logger->error(std::string("Type conversion error: ") + ex.what());
    return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }
}

std::shared_ptr<DBResult> PostgreSQLDB::query(std::string sql, std::vector<std::any> params) {
  auto res = std::make_shared<DBResult>();
  std::string param_strs;

  // Get timing
  auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_connection || !_connection->is_open()) {
      throw std::runtime_error("Connection is not open");
    }

    // Use a write transaction (pqxx::work) since prepared statements are not supported in nontransaction.
    pqxx::work txn(*_connection);

    // Prepare a unique statement name for this query.
    std::string stmt_name = generate_unique_stmt_name();
    _connection->prepare(stmt_name, sql);

    // Build parameter strings for execution.
    std::vector<std::string> paramValues;
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i].type() == typeid(std::string)) {
        auto v = std::any_cast<std::string>(params[i]);
        paramValues.push_back(v);
        param_strs += v + ",";
      } else if (params[i].type() == typeid(int)) {
        auto v = std::any_cast<int>(params[i]);
        paramValues.push_back(std::to_string(v));
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(double)) {
        auto v = std::any_cast<double>(params[i]);
        paramValues.push_back(std::to_string(v));
        param_strs += std::to_string(v) + ",";
      } else {
        throw std::runtime_error("Unsupported parameter type at index " + std::to_string(i));
      }
    }

    // Execute the prepared statement.
    // (While exec_prepared is marked deprecated, it is still available with pqxx::work.)
    pqxx::result result = txn.exec_prepared(stmt_name, paramValues);

    // Unprepare the statement.
    _connection->unprepare(stmt_name);

    // Get timing duration.
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - start)
                        .count() /
                    1000.0;

    // Log SQL
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << duration;
    std::string durationStr = oss.str();
    _logger->debug("SQL: " + sql + " [" + Util::trim(param_strs, ",") + "] (" +
                   std::to_string(result.size()) + " rows, " + durationStr + "ms)");

    // Fetch column names and convert column type OIDs to type names.
    std::vector<std::string> columnNames;
    std::vector<std::string> columnTypes;
    for (int i = 0; i < result.columns(); ++i) {
      columnNames.push_back(result.column_name(i));
      columnTypes.push_back(oid_to_type(result.column_type(i)));
    }

    // Iterate over the fetched rows.
    for (const auto &row : result) {
      auto dbRow = std::make_shared<DBRow>();
      for (pqxx::row::size_type col = 0; col < row.size(); ++col) {
        std::string colName = columnNames[col];
        auto val = _map_value(row[col], columnTypes[col]);
        dbRow->values.insert_or_assign(colName, val);
        dbRow->column_values.push_back(val);
      }
      res->rows.push_back(dbRow);
    }

    // Store affected rows count.
    res->rows_affected = result.affected_rows();

    // Commit the transaction.
    txn.commit();

  } catch (const std::exception &ex) {
    res->error = ex.what();
    _logger->error(std::string("Exception: ") + ex.what());
  } catch (...) {
    res->error = "Unknown exception";
    _logger->error("Unknown exception occurred.");
  }

  return res;
}

void PostgreSQLDB::close() {
  if (_connection && _connection->is_open()) {
    _connection->close(); // Use close() instead of disconnect()
    _connection.reset();
  }
}

// Register with DB Drivers
static bool postgres_registered = [] {
  DB::register_driver<PostgreSQLDB>("postgresql");
  DB::register_driver<PostgreSQLDB>("postgres");
  return true;
}();

}  // namespace athenasip
