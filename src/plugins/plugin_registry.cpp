//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "plugin_registry.h"

#include <algorithm>

#include "../loggers/logger_scoped.h"

namespace athenasip::plugins {

PluginRegistry& PluginRegistry::instance() {
  static PluginRegistry registry;
  return registry;
}

void PluginRegistry::add(std::shared_ptr<loggers::Logger> logger, std::string kind, std::string scheme, Factory factory, Settings settings) {
  auto scoped = std::make_shared<loggers::LoggerScoped>("plugins", std::move(logger));
  scoped->debug("Registering " + kind + " scheme " + scheme);

  std::lock_guard<std::mutex> lock(_mutex);
  _settings[{kind, scheme}] = std::move(settings);
  _factories[{std::move(kind), std::move(scheme)}] = std::move(factory);
}

std::shared_ptr<Plugin> PluginRegistry::create(std::shared_ptr<loggers::Logger> logger, const std::string& kind, const std::string& url_string) const {
  auto scoped = std::make_shared<loggers::LoggerScoped>("plugins", logger);
  auto url = std::make_shared<types::URL>(url_string);

  Factory factory;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _factories.find({kind, url->scheme});
    if (it == _factories.end()) {
      scoped->error("No " + kind + " driver for scheme " + url->scheme);
      return nullptr;
    }

    factory = it->second;
  }

  auto plugin = factory(std::move(logger), std::move(url));
  if (!plugin) {
    scoped->error("The " + kind + " driver for " + url_string + " would not construct");
    return nullptr;
  }

  // A plugin built against a different contract version must not run.
  if (plugin->api_version() != API_VERSION) {
    scoped->error("Plugin " + plugin->describe() + " was built against contract version " + std::to_string(plugin->api_version()) + ", this server speaks " +
                  std::to_string(API_VERSION));
    return nullptr;
  }

  return plugin;
}

std::vector<PluginRegistry::Registration> PluginRegistry::list() const {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<Registration> registrations;
  registrations.reserve(_factories.size());
  for (const auto& [key, factory] : _factories) {
    registrations.push_back({key.first, key.second});
  }

  return registrations;
}

std::vector<std::string> PluginRegistry::schemes(const std::string& kind) const {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::string> schemes;
  for (const auto& [key, factory] : _factories) {
    if (key.first == kind) {
      schemes.push_back(key.second);
    }
  }

  return schemes;
}

Settings PluginRegistry::settings() const {
  std::lock_guard<std::mutex> lock(_mutex);

  Settings all;
  for (const auto& [key, settings] : _settings) {
    for (auto setting : settings) {
      setting.key = key.first + "." + setting.key;
      if (std::none_of(all.begin(), all.end(), [&setting](const Setting& known) { return known.key == setting.key; })) all.push_back(std::move(setting));
    }
  }

  return all;
}

void PluginRegistry::clear() {
  std::lock_guard<std::mutex> lock(_mutex);
  _factories.clear();
  _settings.clear();
}

}  // namespace athenasip::plugins
