//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <any>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <utility>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"

using namespace athenasip::types;
using namespace athenasip::loggers;

namespace athenasip::databases {

// Abstract base class for a database value.
class DBValue {
 public:
  virtual ~DBValue() = default;

  virtual bool is_null() = 0;
  virtual bool is_signed() = 0;

  virtual std::any get_any() const = 0;


template <typename T, typename U>
static T checked_integral_cast(U value) {
  static_assert(std::is_integral_v<T>);
  static_assert(std::is_integral_v<U>);

  if (!std::in_range<T>(value)) {
    std::cout << "[DEVELOPER] DBValue integer cast out of range: "
              << value << " cannot fit in " << typeid(T).name()
              << std::endl;
    return T{};
  }

  return static_cast<T>(value);
}

template <typename T>
T as() const {
  const auto& value = get_any();

  if (const auto* exact = std::any_cast<T>(&value)) {
    return *exact;
  }

  if constexpr (std::is_integral_v<T>) {
    if (const auto* v = std::any_cast<std::int64_t>(&value)) {
      return checked_integral_cast<T>(*v);
    }

    if (const auto* v = std::any_cast<std::uint64_t>(&value)) {
      return checked_integral_cast<T>(*v);
    }
  }

  std::cout << "[DEVELOPER] Bad any cast when getting DBValue: "
            << "cast from " << value.type().name()
            << " to " << typeid(T).name()
            << std::endl;

  return T{};
}
};

// Template implementation for a database value.
template <typename T>
class DBValueImpl : public DBValue {
 private:
  T value;
  bool is_null_flag;

 public:
  explicit DBValueImpl(T v) : value(v), is_null_flag(false) {}
  explicit DBValueImpl() : is_null_flag(true) {}

  bool is_null() override { return is_null_flag; }
  bool is_signed() override { return std::is_signed<T>::value; }

  std::any get_any() const override { return value; }
};

// DBRow now holds its values in a public 'values' field.
class DBRow {
 public:
  // Mapping of column name to a shared pointer to a DBValue.
  std::unordered_map<std::string, std::shared_ptr<DBValue>> values;
  // Mapping by column index
  std::vector<std::shared_ptr<DBValue>> column_values;

  // Provide read-only access to a value by key.
  std::shared_ptr<DBValue> operator[](const std::string& key) const {
    auto it = values.find(key);
    return (it != values.end()) ? it->second : nullptr;
  }
};

// DBResult now holds its rows in a public 'rows' field as a vector of shared_ptr<DBRow>.
class DBResult {
 public:
  std::vector<std::shared_ptr<DBRow>> rows;
  uint32_t rows_affected;
  std::string error;
};

class DB {
 public:
  DB(std::shared_ptr<Logger> logger) : _logger(logger) {};

  virtual bool connect() = 0;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) = 0;
  virtual void close() = 0;
  virtual bool is_created() = 0;

  // Register a Database Driver
  template <typename T, typename = std::enable_if_t<std::is_base_of<DB, T>::value>>
  static void register_driver(std::shared_ptr<Logger> logger, std::string scheme) {
    auto& drivers = get_drivers();
    logger->debug("(dbcore) Registering scheme " + scheme);
    drivers[scheme] = [](std::shared_ptr<Logger> logger, std::shared_ptr<URL> url) -> std::shared_ptr<DB> {
      return std::static_pointer_cast<DB>(std::make_shared<T>(logger, url));
    };
  }

  // Get a driver instance by name
  static std::shared_ptr<DB> create_driver(std::shared_ptr<Logger> logger, std::string url) {
    auto _url = std::make_shared<URL>(url);
    logger->debug("(dbcore) Finding scheme " + _url->scheme);
    auto& drivers = get_drivers();
    auto it = drivers.find(_url->scheme);
    if (it != drivers.end()) {
      return it->second(logger, _url);  // Call the stored factory function
    } else {
      logger->error("(dbcore) Unknown Scheme " + _url->scheme);
      return nullptr;
    }
  }

 protected:
  std::shared_ptr<Logger> _logger;

  static std::unordered_map<std::string, std::function<std::shared_ptr<DB>(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)>>& get_drivers() {
    static std::unordered_map<std::string, std::function<std::shared_ptr<DB>(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)>> drivers;
    return drivers;
  }
};

}  // namespace athenasip::databases
