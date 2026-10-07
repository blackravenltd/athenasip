//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../types/url.h"
#include "plugin.h"
#include "setting.h"

namespace athenasip::plugins {

// The one registry for every kind of plugin, keyed by (kind, scheme). Built-in drivers
// register here exactly as external ones do.
class PluginRegistry {
 public:
  using Factory = std::function<std::shared_ptr<Plugin>(std::shared_ptr<loggers::Logger>, std::shared_ptr<types::URL>)>;

  struct Registration {
    std::string kind;
    std::string scheme;
  };

  static PluginRegistry& instance();

  // settings: the driver's own section, if it describes one (see Setting::key).
  void add(std::shared_ptr<loggers::Logger> logger, std::string kind, std::string scheme, Factory factory, Settings settings = {});

  // A driver describes its section with a static `plugins::Settings settings()`, which is
  // optional.
  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Plugin, T>>>
  void add(std::shared_ptr<loggers::Logger> logger, std::string kind, std::string scheme) {
    Settings settings;
    if constexpr (requires { T::settings(); }) settings = T::settings();

    add(
        std::move(logger), std::move(kind), std::move(scheme),
        [](std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) -> std::shared_ptr<Plugin> {
          return std::static_pointer_cast<Plugin>(std::make_shared<T>(std::move(logger), std::move(url)));
        },
        std::move(settings));
  }

  // Constructs the driver registered for this kind and the URL's scheme. Returns nullptr
  // when none is registered or its contract version does not match.
  std::shared_ptr<Plugin> create(std::shared_ptr<loggers::Logger> logger, const std::string& kind, const std::string& url_string) const;

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Plugin, T>>>
  std::shared_ptr<T> create_as(std::shared_ptr<loggers::Logger> logger, const std::string& kind, const std::string& url_string) const {
    // By kind, not dynamic_cast: a plugin built as a module carries its own copy of the interface's type
    // information, which need not compare equal to the server's.
    auto plugin = create(std::move(logger), kind, url_string);
    return plugin && plugin->kind() == kind ? std::static_pointer_cast<T>(plugin) : nullptr;
  }

  // Everything registered, for diagnostics.
  std::vector<Registration> list() const;
  std::vector<std::string> schemes(const std::string& kind) const;

  // What every registered driver says of its section, keyed from the top of the file
  // ("media.rtpengine.timeout_ms"). A driver reached by more than one scheme says it once.
  Settings settings() const;

  // For tests that register drivers of their own.
  void clear();

 private:
  mutable std::mutex _mutex;
  std::map<std::pair<std::string, std::string>, Factory> _factories;
  std::map<std::pair<std::string, std::string>, Settings> _settings;
};

}  // namespace athenasip::plugins
