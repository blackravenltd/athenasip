//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"
#include "../types/url.h"

namespace athenasip::events {

class TopicFilter {
 public:
  static bool is_filter(std::string_view value) { return value.find_first_of("+#") != std::string_view::npos; }

  static bool is_valid_topic(std::string_view topic) {
    if (topic.empty()) {
      return false;
    }

    return topic.find_first_of("+#") == std::string_view::npos;
  }

  static bool is_valid_filter(std::string_view filter) {
    if (filter.empty()) {
      return false;
    }

    const auto levels = split(filter);
    for (std::size_t i = 0; i < levels.size(); ++i) {
      const auto level = levels[i];

      if (level.find('#') != std::string_view::npos) {
        if (level != "#" || i != levels.size() - 1) {
          return false;
        }
      }

      if (level.find('+') != std::string_view::npos && level != "+") {
        return false;
      }
    }

    return true;
  }

  static bool matches(std::string_view filter, std::string_view topic) {
    if (!is_valid_filter(filter) || !is_valid_topic(topic)) {
      return false;
    }

    // As in MQTT, a leading wildcard does not match a topic beginning with '$'.
    if (!topic.empty() && topic.front() == '$' && !filter.empty() && (filter.front() == '#' || filter.front() == '+')) {
      return false;
    }

    const auto filter_levels = split(filter);
    const auto topic_levels = split(topic);

    for (std::size_t i = 0; i < filter_levels.size(); ++i) {
      const auto filter_level = filter_levels[i];

      if (filter_level == "#") {
        return i == filter_levels.size() - 1;
      }

      if (i >= topic_levels.size()) {
        return false;
      }

      if (filter_level == "+") {
        continue;
      }

      if (filter_level != topic_levels[i]) {
        return false;
      }
    }

    return filter_levels.size() == topic_levels.size();
  }

 private:
  static std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> levels;

    std::size_t start = 0;
    while (start <= value.size()) {
      const auto pos = value.find('/', start);
      if (pos == std::string_view::npos) {
        levels.push_back(value.substr(start));
        break;
      }

      levels.push_back(value.substr(start, pos - start));
      start = pos + 1;
    }

    return levels;
  }
};

class Subscription {
 public:
  using EventCallbackFn = std::function<void(std::string, std::string)>;

  const std::string event_name;
  const EventCallbackFn callback;

  Subscription(std::string _event_name, EventCallbackFn _callback) : event_name(std::move(_event_name)), callback(std::move(_callback)) {}
};

// The event bus: observability, presence and discovery. It is never on the call setup
// path. Operations that can be waited on take the caller's executor and answer through a
// handler, as Datastore and MediaEngine do.
class EventSystem : public plugins::Plugin {
 public:
  ~EventSystem() override = default;

  std::string kind() const final { return plugins::kinds::events; }

  // connect() answers through the handler; close() is synchronous and safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // Fire and forget: never blocks and never reports failure; a driver logs what it could
  // not send.
  virtual void publish(std::string event_name, std::string message) = 0;

  // A publish whose caller wants to hear whether the bus accepted it.
  virtual void publish(plugins::Executor on, std::string event_name, std::string message, plugins::StatusHandler handler) = 0;

  // Retained state: the last message is kept and delivered to whoever subscribes later.
  // A bus that cannot retain falls back to an ordinary publish.
  virtual void publish_state(std::string event_name, std::string message) { publish(std::move(event_name), std::move(message)); }

  // The message the bus publishes on this node's behalf if the node vanishes (the MQTT
  // will). Call before connect(); it has no effect afterwards. Optional.
  virtual void will_set(std::string event_name, std::string message) {
    (void)event_name;
    (void)message;
  }

  // The subscription handle arrives through the handler; keep it to unsubscribe.
  virtual void subscribe(plugins::Executor on, std::string event_name, Subscription::EventCallbackFn event_callback,
                         plugins::Handler<std::shared_ptr<Subscription>> handler) = 0;

  virtual void unsubscribe(plugins::Executor on, std::shared_ptr<Subscription> subscription, plugins::StatusHandler handler) = 0;
  virtual void unsubscribe_all(plugins::Executor on, plugins::StatusHandler handler) = 0;

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<EventSystem, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), plugins::kinds::events, std::move(scheme));
  }

  static std::shared_ptr<EventSystem> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<EventSystem>(std::move(logger), plugins::kinds::events, url_string);
  }
};

}  // namespace athenasip::events
