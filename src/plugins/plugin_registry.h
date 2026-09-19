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

namespace athenasip::plugins {

// One registry for every kind of plugin, keyed by (kind, scheme). There were three
// copy-pasted template registries before this, one per interface, which was fine for
// three kinds and the wrong shape for a server whose plugin contract is the product:
// adding a kind meant adding a registry.
//
// A built-in driver registers here exactly as an external one will. The only
// difference is link time against load time.
class PluginRegistry {
 public:
  using Factory = std::function<std::shared_ptr<Plugin>(std::shared_ptr<loggers::Logger>, std::shared_ptr<types::URL>)>;

  struct Registration {
    std::string kind;
    std::string scheme;
  };

  static PluginRegistry& instance();

  void add(std::shared_ptr<loggers::Logger> logger, std::string kind, std::string scheme, Factory factory);

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Plugin, T>>>
  void add(std::shared_ptr<loggers::Logger> logger, std::string kind, std::string scheme) {
    add(std::move(logger), std::move(kind), std::move(scheme),
        [](std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) -> std::shared_ptr<Plugin> {
          return std::static_pointer_cast<Plugin>(std::make_shared<T>(std::move(logger), std::move(url)));
        });
  }

  // Construct the driver registered for this kind and the URL's scheme. Returns
  // nullptr when no driver is registered, or when the one that is was built against a
  // different contract version.
  std::shared_ptr<Plugin> create(std::shared_ptr<loggers::Logger> logger, const std::string& kind, const std::string& url_string) const;

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Plugin, T>>>
  std::shared_ptr<T> create_as(std::shared_ptr<loggers::Logger> logger, const std::string& kind, const std::string& url_string) const {
    return std::dynamic_pointer_cast<T>(create(std::move(logger), kind, url_string));
  }

  // Everything registered, for diagnostics and for `athenasip plugins list` later.
  std::vector<Registration> list() const;
  std::vector<std::string> schemes(const std::string& kind) const;

  // Tests register drivers of their own; this puts the registry back.
  void clear();

 private:
  mutable std::mutex _mutex;
  std::map<std::pair<std::string, std::string>, Factory> _factories;
};

}  // namespace athenasip::plugins
