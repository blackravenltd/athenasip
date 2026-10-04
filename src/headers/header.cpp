//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "string_header.h"

namespace athenasip::headers {

// Builds the registered type for the field name, falling back to StringHeader when there is none or it will not parse.
std::shared_ptr<Header> Header::create(const std::string& fieldName, const std::string& value) {
  auto& registry = get_registry();
  auto it = registry.find(fieldName);
  if (it != registry.end()) {
    auto instance = (it->second)();
    if (instance && instance->parse(value)) {
      return instance;
    }
  }
  auto defaultInstance = std::make_shared<StringHeader>();
  defaultInstance->parse(value);
  return defaultInstance;
}

void Header::register_factory(const std::string& fieldName, std::function<std::shared_ptr<Header>()> factory) { get_registry()[fieldName] = std::move(factory); }

std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& Header::get_registry() {
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>> registry;
  return registry;
}

}  // namespace athenasip::headers