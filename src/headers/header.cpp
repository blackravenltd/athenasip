//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "string_header.h"

namespace athenasip::headers {

// Factory method: creates an instance for a given field name and parses the provided value.
std::shared_ptr<Header> Header::create(const std::string& fieldName, const std::string& value) {
  auto& registry = getRegistry();
  auto it = registry.find(fieldName);
  if (it != registry.end()) {
    // Call the factory to get a new Header instance.
    auto instance = (it->second)();
    if (instance && instance->parse(value)) {
      return instance;
    }
  }
  // Fallback to a default StringHeader.
  auto defaultInstance = std::make_shared<StringHeader>();
  defaultInstance->parse(value);
  return defaultInstance;
}

// Registration method: associates a field name with a factory function.
void Header::register_factory(const std::string& fieldName, std::function<std::shared_ptr<Header>()> factory) { getRegistry()[fieldName] = std::move(factory); }

// Accessor for the static registry map.
std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& Header::getRegistry() {
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>> registry;
  return registry;
}

}  // namespace athenasip::headers