//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "field_value.h"

#include <iostream>

#include "string_field_value.h"

namespace athenasip::sipfields {

// Factory method: creates an instance for a given field name and parses the provided value.
std::shared_ptr<FieldValue> FieldValue::create(const std::string& fieldName, const std::string& value) {
  auto& registry = getRegistry();
  auto it = registry.find(fieldName);
  if (it != registry.end()) {
    // Call the factory to get a new FieldValue instance.

    std::cout << "Found Factory for field " << fieldName << ": " << std::endl;
    auto instance = (it->second)();
    if (instance && instance->parse(value)) {
      return instance;
    }
  }
  std::cout << "Using default StringFieldValue  Factory for field " << fieldName << ": " << std::endl;
  // Fallback to a default StringFieldValue.
  auto defaultInstance = std::make_shared<StringFieldValue>();
  defaultInstance->parse(value);
  return defaultInstance;
}

// Registration method: associates a field name with a factory function.
void FieldValue::register_factory(const std::string& fieldName, std::function<std::shared_ptr<FieldValue>()> factory) {
  getRegistry()[fieldName] = std::move(factory);
}

// Accessor for the static registry map.
std::unordered_map<std::string, std::function<std::shared_ptr<FieldValue>()>>& FieldValue::getRegistry() {
  static std::unordered_map<std::string, std::function<std::shared_ptr<FieldValue>()>> registry;
  return registry;
}

}  // namespace athenasip::sipfields