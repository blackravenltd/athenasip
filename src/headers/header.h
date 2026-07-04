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

// Abstract plus factory
class Header {
 public:
  virtual ~Header() = default;

  // Pure virtual interface.
  virtual bool parse(const std::string& value) = 0;
  virtual std::string to_string() const = 0;

  template <typename T>
  T* as() {
    return dynamic_cast<T*>(this);
  }

  // Factory method: creates an instance for a given field name and parses the provided value.
  static std::shared_ptr<Header> create(const std::string& fieldName, const std::string& value);

  // Registration method: associates a field name with a factory function.
  static void register_factory(const std::string& fieldName, std::function<std::shared_ptr<Header>()> factory);

 protected:
  // Accessor for the static registry map.
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& getRegistry();
};

}  // namespace athenasip::headers