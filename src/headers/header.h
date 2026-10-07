//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace athenasip::headers {

// A parsed header field. Field types register a factory by name; unregistered fields are StringHeader.
class Header {
 public:
  virtual ~Header() = default;

  virtual bool parse(const std::string& value) = 0;
  virtual std::string to_string() const = 0;

  template <typename T>
  T* as() {
    return dynamic_cast<T*>(this);
  }

  // Builds the registered type for the field name and parses the value into it.
  static std::shared_ptr<Header> create(const std::string& fieldName, const std::string& value);

  static void register_factory(const std::string& fieldName, std::function<std::shared_ptr<Header>()> factory);

 protected:
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& get_registry();
};

}  // namespace athenasip::headers