//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "postgres_db.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

namespace athenasip::databases {
namespace {

std::string oid_to_type(pqxx::oid oid) {
  switch (oid) {
    case 21:
      return "int2";
    case 23:
      return "int4";
    case 20:
      return "int8";
    case 700:
      return "float4";
    case 701:
      return "float8";
    case 1700:
      return "numeric";
    case 16:
      return "bool";
    default:
      return "text";
  }
}

template <typename T>
bool append_if_type(const std::any& value, pqxx::params& params, std::string& log) {
  if (value.type() != typeid(T)) {
    return false;
  }

  const auto& typed = std::any_cast<const T&>(value);
  params.append(typed);

  std::ostringstream oss;
  oss << typed;
  log += oss.str();
  log += ",";

  return true;
}

bool append_string_if_type(const std::any& value, pqxx::params& params, std::string& log) {
  if (value.type() == typeid(std::string)) {
    const auto& typed = std::any_cast<const std::string&>(value);
    params.append(typed);
    log += typed;
    log += ",";
    return true;
  }

  if (value.type() == typeid(std::string_view)) {
    const auto typed = std::any_cast<std::string_view>(value);
    const std::string copied{typed};
    params.append(copied);
    log += copied;
    log += ",";
    return true;
  }

  if (value.type() == typeid(const char*)) {
    const auto typed = std::any_cast<const char*>(value);
    if (typed == nullptr) {
      params.append();
      log += "NULL,";
    } else {
      const std::string copied{typed};
      params.append(copied);
      log += copied;
      log += ",";
    }
    return true;
  }

  if (value.type() == typeid(char*)) {
    const auto typed = std::any_cast<char*>(value);
    if (typed == nullptr) {
      params.append();
      log += "NULL,";
    } else {
      const std::string copied{typed};
      params.append(copied);
      log += copied;
      log += ",";
    }
    return true;
  }

  return false;
}

void append_param(const std::any& value, pqxx::params& params, std::string& log, std::size_t index) {
  if (!value.has_value() || value.type() == typeid(std::nullptr_t)) {
    params.append();
    log += "NULL,";
    return;
  }

  if (append_string_if_type(value, params, log)) {
    return;
  }

  if (append_if_type<bool>(value, params, log)) {
    return;
  }

  if (append_if_type<int>(value, params, log) || append_if_type<unsigned int>(value, params, log) || append_if_type<long>(value, params, log) ||
      append_if_type<unsigned long>(value, params, log) || append_if_type<long long>(value, params, log) ||
      append_if_type<unsigned long long>(value, params, log) || append_if_type<std::int16_t>(value, params, log) ||
      append_if_type<std::uint16_t>(value, params, log) || append_if_type<std::int32_t>(value, params, log) ||
      append_if_type<std::uint32_t>(value, params, log) || append_if_type<std::int64_t>(value, params, log) ||
      append_if_type<std::uint64_t>(value, params, log) || append_if_type<float>(value, params, log) || append_if_type<double>(value, params, log) ||
      append_if_type<long double>(value, params, log)) {
    return;
  }

  throw std::runtime_error("Unsupported PostgreSQL parameter type at index " + std::to_string(index) + ": " + value.type().name());
}

std::uint32_t clamp_rows_affected(pqxx::result::size_type rows) {
  const auto max = static_cast<pqxx::result::size_type>(std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(rows > max ? max : rows);
}

}  // namespace

PostgreSQLDB::PostgreSQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)
    : DB(std::make_shared<LoggerScoped>("postgres", logger)), _url(std::move(url)) {}

PostgreSQLDB::~PostgreSQLDB() { close(); }

bool PostgreSQLDB::connect() {
  _url->scheme = "postgresql";
  _logger->debug("PostgreSQL URL: " + _url->to_string());

  try {
    _connection = std::make_shared<pqxx::connection>(_url->to_string());

    if (!_connection->is_open()) {
      _logger->error("Connection to PostgreSQL failed.");
      return false;
    }
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    return false;
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    return false;
  }

  return true;
}

std::shared_ptr<DBValue> PostgreSQLDB::_map_value(pqxx::field_ref field, const std::string& colType) {
  if (field.is_null()) {
    return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }

  try {
    if (colType == "int2" || colType == "int4" || colType == "int8") {
      return std::make_shared<DBValueImpl<std::int64_t>>(field.as<std::int64_t>());
    }

    if (colType == "float4" || colType == "float8" || colType == "numeric") {
      return std::make_shared<DBValueImpl<double>>(field.as<double>());
    }

    if (colType == "bool") {
      return std::make_shared<DBValueImpl<bool>>(field.as<bool>());
    }

    return std::make_shared<DBValueImpl<std::string>>(field.as<std::string>());
  } catch (const std::exception& ex) {
    _logger->error(std::string("Type conversion error: ") + ex.what());
    return std::make_shared<DBValueImpl<std::nullptr_t>>();
  }
}

std::shared_ptr<DBResult> PostgreSQLDB::query(std::string sql, std::vector<std::any> params) {
  auto res = std::make_shared<DBResult>();
  res->rows_affected = 0;

  std::string param_strs;
  const auto start = std::chrono::high_resolution_clock::now();

  try {
    if (!_connection || !_connection->is_open()) {
      throw std::runtime_error("Connection is not open");
    }

    pqxx::work txn(*_connection);
    pqxx::params param_values{txn};

    for (std::size_t i = 0; i < params.size(); ++i) {
      append_param(params[i], param_values, param_strs, i);
    }

    pqxx::result result = params.empty() ? txn.exec(sql) : txn.exec(sql, param_values);

    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << duration;

    _logger->debug("SQL: " + sql + " [" + Util::trim(param_strs, ",") + "] (" + std::to_string(result.size()) + " rows, " + oss.str() + "ms)");

    std::vector<std::string> column_names;
    std::vector<std::string> column_types;
    column_names.reserve(static_cast<std::size_t>(result.columns()));
    column_types.reserve(static_cast<std::size_t>(result.columns()));

    for (pqxx::row_size_type i = 0; i < result.columns(); ++i) {
      column_names.emplace_back(result.column_name(i));
      column_types.emplace_back(oid_to_type(result.column_type(i)));
    }

    for (auto row : result) {
      auto db_row = std::make_shared<DBRow>();

      for (pqxx::row_size_type col = 0; col < row.size(); ++col) {
        auto value = _map_value(row[col], column_types[static_cast<std::size_t>(col)]);
        const auto& column_name = column_names[static_cast<std::size_t>(col)];

        db_row->values.insert_or_assign(column_name, value);
        db_row->column_values.push_back(value);
      }

      res->rows.push_back(db_row);
    }

    res->rows_affected = clamp_rows_affected(result.affected_rows());

    txn.commit();
  } catch (const std::exception& ex) {
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
    _connection->close();
    _connection.reset();
  }
}

bool PostgreSQLDB::is_created() { return true; }

}  // namespace athenasip::databases
