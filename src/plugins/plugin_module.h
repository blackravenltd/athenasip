//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "../loggers/logger.h"
#include "plugin.h"
#include "plugin_registry.h"

// A plugin built as a shared library (a module): what it exports, and what the server hands it. Build it against
// these headers with the same compiler and standard library as the server, because std::shared_ptr and
// YAML::Node cross the boundary; docs/plugins.md has the rest.

#if defined(_WIN32)
#define ATHENASIP_MODULE_EXPORT __declspec(dllexport)
#else
#define ATHENASIP_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace athenasip::plugins {

// What the server lends a module while it registers: where its drivers go, and a logger.
class ModuleHost {
 public:
  virtual ~ModuleHost() = default;

  // As PluginRegistry::add: the factory constructs the driver for a URL with this scheme, and the settings
  // describe its section.
  virtual void add(std::string kind, std::string scheme, PluginRegistry::Factory factory, Settings settings) = 0;

  virtual std::shared_ptr<loggers::Logger> logger() = 0;

  // A driver describes its section with an optional static `plugins::Settings settings()`, as a built-in one does.
  template <typename T>
  void add(std::string kind, std::string scheme) {
    Settings settings;
    if constexpr (requires { T::settings(); }) settings = T::settings();

    add(
        std::move(kind), std::move(scheme),
        [](std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) -> std::shared_ptr<Plugin> {
          return std::static_pointer_cast<Plugin>(std::make_shared<T>(std::move(logger), std::move(url)));
        },
        std::move(settings));
  }
};

}  // namespace athenasip::plugins

// Declares a module: its name, and the function that registers its drivers with the host. The contract version
// it was built against is exported with it, and a server speaking another refuses to load it.
//
//   void register_acme(athenasip::plugins::ModuleHost& host) { host.add<AcmeDatastore>("datastore", "acme"); }
//   ATHENASIP_PLUGIN_MODULE("acme", register_acme)
#define ATHENASIP_PLUGIN_MODULE(module_name, register_function)                                                               \
  extern "C" ATHENASIP_MODULE_EXPORT std::uint32_t athenasip_module_api_version() { return athenasip::plugins::API_VERSION; } \
  extern "C" ATHENASIP_MODULE_EXPORT const char* athenasip_module_name() { return module_name; }                              \
  extern "C" ATHENASIP_MODULE_EXPORT void athenasip_module_register(athenasip::plugins::ModuleHost* host) { register_function(*host); }
