//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <any>
#include <iostream>
#include <memory>
#include <type_traits>
#include <unordered_map>

class DBValue {
 public:
  virtual ~DBValue() = default;

  virtual bool is_null() = 0;
  virtual bool is_signed() = 0;

  virtual std::any get_any() const = 0;

  template <typename T>
  T get_as() const {
    return std::any_cast<T>(get_any());
  }
};

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

class DBRow : public std::unordered_map<std::string, std::shared_ptr<DBValue>> {
 public:
  std::shared_ptr<DBValue> operator[](const std::string& key) const {
    auto it = this->find(key);
    if (it != this->end()) {
      return it->second;
    }
    return nullptr;  // ✅ Return `nullptr` instead of throwing
  }
};

class DBResult : public std::vector<std::shared_ptr<DBRow>> {
 public:
  uint32_t rows_affected;
  std::string error;
};
