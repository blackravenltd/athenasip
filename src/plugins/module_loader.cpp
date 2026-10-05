//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "module_loader.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include "../loggers/logger_scoped.h"
#include "plugin_module.h"

namespace athenasip::plugins {

namespace {

using ApiVersionFn = std::uint32_t (*)();
using NameFn = const char* (*)();
using RegisterFn = void (*)(ModuleHost*);

#if defined(_WIN32)
using Handle = HMODULE;
Handle open_library(const std::string& path, std::string& error) {
  auto handle = LoadLibraryA(path.c_str());
  if (handle == nullptr) error = "LoadLibrary failed with error " + std::to_string(GetLastError());
  return handle;
}
void* symbol(Handle handle, const char* name) { return reinterpret_cast<void*>(GetProcAddress(handle, name)); }
void close_library(Handle handle) { FreeLibrary(handle); }
#else
using Handle = void*;
Handle open_library(const std::string& path, std::string& error) {
  // RTLD_LOCAL: one module's symbols do not satisfy another's.
  auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) error = dlerror();
  return handle;
}
void* symbol(Handle handle, const char* name) { return dlsym(handle, name); }
void close_library(Handle handle) { dlclose(handle); }
#endif

bool is_module_file(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  return extension == ".so" || extension == ".dylib" || extension == ".dll";
}

// Hands a module's registrations to the registry, and remembers what they were for the report.
class RegistryHost : public ModuleHost {
 public:
  explicit RegistryHost(std::shared_ptr<loggers::Logger> logger) : _logger(std::move(logger)) {}

  void add(std::string kind, std::string scheme, PluginRegistry::Factory factory) override {
    registered.push_back(kind + " " + scheme + "://");
    PluginRegistry::instance().add(_logger, std::move(kind), std::move(scheme), std::move(factory));
  }

  std::shared_ptr<loggers::Logger> logger() override { return _logger; }

  std::vector<std::string> registered;

 private:
  std::shared_ptr<loggers::Logger> _logger;
};

ModuleReport load_one(const std::shared_ptr<loggers::Logger>& logger, const std::filesystem::path& path) {
  ModuleReport report;
  report.path = path.string();

  std::string error;
  auto handle = open_library(report.path, error);
  if (handle == nullptr) {
    report.detail = "could not be opened: " + error;
    return report;
  }

  const auto api_version = reinterpret_cast<ApiVersionFn>(symbol(handle, "athenasip_module_api_version"));
  const auto name = reinterpret_cast<NameFn>(symbol(handle, "athenasip_module_name"));
  const auto register_module = reinterpret_cast<RegisterFn>(symbol(handle, "athenasip_module_register"));

  if (api_version == nullptr || name == nullptr || register_module == nullptr) {
    close_library(handle);
    report.detail = "is not an AthenaSIP module: it does not declare ATHENASIP_PLUGIN_MODULE";
    return report;
  }

  // Read before anything closes the library: both are code and data in it.
  report.name = name();
  const auto built_against = api_version();

  // Its drivers would be built against another contract, which the registry would refuse one by one anyway.
  if (built_against != API_VERSION) {
    close_library(handle);
    report.detail = "was built against contract version " + std::to_string(built_against) + ", this server speaks " + std::to_string(API_VERSION);
    return report;
  }

  RegistryHost host(logger);
  register_module(&host);

  // Never closed: the registry's factories are code in the module.
  report.loaded = true;
  if (host.registered.empty()) {
    report.detail = "registered nothing";
  } else {
    for (const auto& one : host.registered) report.detail += (report.detail.empty() ? "registered " : ", ") + one;
  }
  return report;
}

}  // namespace

std::vector<ModuleReport> load_modules(std::shared_ptr<loggers::Logger> logger, const std::vector<std::string>& directories) {
  auto scoped = std::make_shared<loggers::LoggerScoped>("plugins", logger);
  std::vector<ModuleReport> reports;

  for (const auto& directory : directories) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
      if (entry.is_regular_file(ec) && is_module_file(entry.path())) files.push_back(entry.path());
    }
    if (ec) {
      scoped->warn("Cannot read plugins.path " + directory + " - " + ec.message());
      continue;
    }

    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      auto report = load_one(logger, file);
      if (report.loaded) {
        scoped->info("Loaded plugin module " + report.name + " from " + report.path + ": " + report.detail);
      } else {
        scoped->warn("Did not load " + report.path + " - it " + report.detail);
      }
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

}  // namespace athenasip::plugins
