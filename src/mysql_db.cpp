//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mysql_db.h"

namespace athenasip {

MySQLDB::MySQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url) : DB(std::make_shared<LoggerScoped>("mysql", logger)), _url(url) {}
MySQLDB::~MySQLDB() { close(); }

bool MySQLDB::connect() {
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

  try {
    // Prepare the statement
    mysqlx::SqlStatement stmt = _session->sql(sql);

    // Bind the parameter values
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i].type() == typeid(std::string)) {
        stmt.bind(std::any_cast<std::string>(params[i]));
      } else if (params[i].type() == typeid(int)) {
        stmt.bind(std::any_cast<int>(params[i]));
      } else if (params[i].type() == typeid(double)) {
        stmt.bind(std::any_cast<double>(params[i]));
      } else {
        throw std::runtime_error("Unsupported parameter type at index " + std::to_string(i));
      }
    }

    // Execute the query
    mysqlx::SqlResult result = stmt.execute();

    // // Store affected rows count
    // res->rows_affected = result.getAffectedItemsCount();

    // Ensure query execution is complete before fetching data
    if (!result.hasData()) {
      _logger->debug("Query returned no rows.");
      return res;
    }

    // Fetch column names before iterating over rows
    std::vector<std::string> columnNames;
    for (const auto &col : result.getColumns()) {
      columnNames.push_back(col.getColumnName());
    }

    // Fetch all rows at once into a vector
    std::vector<mysqlx::Row> rows = result.fetchAll();

    // Iterate over the fetched rows
    for (const mysqlx::Row &row : rows) {
      auto dbRow = std::make_shared<DBRow>();

      // Iterate over columns using stored column names
      for (size_t col = 0; col < row.colCount(); ++col) {
        std::string colName = columnNames[col];

        // Map Value
        dbRow->insert_or_assign(colName, _map_value(row[col]));
      }

      res->push_back(dbRow);
    }

  } catch (const mysqlx::Error &err) {
    res->error = err.what();
    _logger->error(std::string("Error: ") + err.what());
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
  return true;
}();

}  // namespace athenasip
