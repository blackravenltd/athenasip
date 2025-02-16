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

namespace athenasip {

// Abstract base class for a database value.
class DBValue {
 public:
  virtual ~DBValue() = default;

  virtual bool is_null() = 0;
  virtual bool is_signed() = 0;

  virtual std::any get_any() const = 0;

  template <typename T>
  T as() const {
    return std::any_cast<T>(get_any());
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

}  // namespace athenasip