//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mysql_db.h"

namespace athenasip::databases {

MySQLDB::MySQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url) : DB(std::make_shared<LoggerScoped>("mysql", logger)), _url(url) {}

bool MySQLDB::connect() {
  // No idea why the MySQL team changed this.
  _url->scheme = "mysqlx";
  _logger->debug("MySQL URL: " + _url->to_string());
  try {
    _session = std::make_shared<mysqlx::Session>(_url->to_string());
  } catch (const mysqlx::Error &err) {
    _logger->error(std::string("Error while connecting: ") + err.what());
    return false;
  } catch (std::exception &ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    return false;
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    return false;
  }
  return true;
}

std::shared_ptr<DBValue> MySQLDB::_map_value(const mysqlx::Value &val) {
  if (val.isNull()) {
    return std::make_shared<DBValueImpl<std::nullptr_t>>();  // Null value
  }

  switch (val.getType()) {
    case mysqlx::Value::Type::STRING:
      return std::make_shared<DBValueImpl<std::string>>(val.get<std::string>());
    case mysqlx::Value::Type::INT64:
      return std::make_shared<DBValueImpl<int64_t>>(val.get<int64_t>());
    case mysqlx::Value::Type::UINT64:
      return std::make_shared<DBValueImpl<uint64_t>>(val.get<uint64_t>());
    case mysqlx::Value::Type::FLOAT:
      return std::make_shared<DBValueImpl<float>>(val.get<float>());
    case mysqlx::Value::Type::DOUBLE:
      return std::make_shared<DBValueImpl<double>>(val.get<double>());
    case mysqlx::Value::Type::BOOL:
      return std::make_shared<DBValueImpl<bool>>(val.get<bool>());
    default:
      _logger->error("Unsupported column type: " + std::to_string(static_cast<int>(val.getType())));
      return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }
}

std::shared_ptr<DBResult> MySQLDB::query(std::string sql, std::vector<std::any> params) {
  auto res = std::make_shared<DBResult>();

  std::string param_strs;

  // Get Timing
  auto start = std::chrono::high_resolution_clock::now();

  // Prepare the statement & Bind the parameter values
  try {
    mysqlx::SqlStatement stmt = _session->sql(sql);

    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i].type() == typeid(std::string)) {
        auto v = std::any_cast<std::string>(params[i]);
        stmt.bind(v);
        param_strs += v + ",";
      } else if (params[i].type() == typeid(uint8_t)) {
        auto v = std::any_cast<uint8_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(uint16_t)) {
        auto v = std::any_cast<uint16_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(uint32_t)) {
        auto v = std::any_cast<uint32_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(uint64_t)) {
        auto v = std::any_cast<uint64_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(int)) {
        auto v = std::any_cast<int>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(int16_t)) {
        auto v = std::any_cast<int16_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(int32_t)) {
        auto v = std::any_cast<int32_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(int64_t)) {
        auto v = std::any_cast<int64_t>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(float)) {
        auto v = std::any_cast<float>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(double)) {
        auto v = std::any_cast<double>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else if (params[i].type() == typeid(char)) {
        auto v = std::any_cast<char>(params[i]);
        stmt.bind(v);
        param_strs += std::to_string(v) + ",";
      } else {
        throw std::runtime_error("Unsupported parameter type at index " + std::to_string(i) + " (? Name " + params[i].type().name() + ")");
      }
    }

    // Execute the query
    mysqlx::SqlResult result = stmt.execute();

    // Fetch column names before iterating over rows
    std::vector<std::string> columnNames;
    for (const auto &col : result.getColumns()) {
      columnNames.push_back(col.getColumnName());
    }

    // Fetch all rows at once into a vector
    std::vector<mysqlx::Row> rows = result.fetchAll();

    // Get Timing
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;

    // Log SQL
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << duration;
    std::string durationStr = oss.str();
    _logger->debug("SQL: " + sql + " [" + Util::trim(param_strs, ",") + "] (" + std::to_string(rows.size()) + " rows, " + durationStr + "ms)");

    // Store affected rows count
    res->rows_affected = result.getAffectedItemsCount();

    // Iterate over the fetched rows
    for (const mysqlx::Row &row : rows) {
      auto dbRow = std::make_shared<DBRow>();

      // Iterate over columns using stored column names
      for (size_t col = 0; col < row.colCount(); ++col) {
        std::string colName = columnNames[col];

        // Map Value
        auto val = _map_value(row[col]);
        dbRow->values.insert_or_assign(colName, val);
        dbRow->column_values.push_back(val);
      }

      res->rows.push_back(dbRow);
    }

  } catch (const mysqlx::Error &err) {
    res->error = err.what();
    _logger->error("SQL: " + sql + " [" + Util::trim(param_strs, ",") + "] Error: " + err.what());
  } catch (std::exception &ex) {
    res->error = ex.what();
    _logger->error(std::string("Standard Exception: ") + ex.what());
  } catch (...) {
    res->error = "Unknown exception";
    _logger->error("Unknown exception occurred.");
  }

  return res;
}

void MySQLDB::close() {
  _session->close();
  _session.reset();
}

// Register with DB Drivers
static bool mysql_registered = [] {
  DB::register_driver<MySQLDB>("mysqlx");
  DB::register_driver<MySQLDB>("mysql");
  return true;
}();

}  // namespace athenasip::databases
