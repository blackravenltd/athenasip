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

    // MQTT system topics are not matched by a leading wildcard. AthenaSIP
    // topics should not normally use '$', but keep the semantics compatible.
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
// path, which is what makes the fire-and-forget publish below legitimate.
//
// Every operation that can be waited on takes the caller's executor and answers through
// a handler, the same way Datastore and MediaEngine do. A broker is a network round trip
// and the caller is usually on the Core strand, which must never wait; and one contract
// the whole of which is shaped the same way is one thing for a plugin author to learn.
class EventSystem : public plugins::Plugin {
 public:
  ~EventSystem() override = default;

  std::string kind() const final { return plugins::kinds::events; }

  // Lifecycle, as Datastore and MediaEngine have it: connect() is a round trip for a
  // networked bus and answers through the handler; close() is teardown, synchronous and
  // safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // Fire and forget, and the reason the bus is not simply async like everything else:
  // a node announcing a channel or a transaction has nothing to do with the answer, and
  // making it wait for one would put the broker on the path of the call that caused it.
  // It never blocks and never fails loudly; a driver logs what it could not send.
  virtual void publish(std::string event_name, std::string message) = 0;

  // The same publish for a caller that does care - provisioning, an admin action, a
  // test - and is willing to hear that the broker refused it.
  virtual void publish(plugins::Executor on, std::string event_name, std::string message, plugins::StatusHandler handler) = 0;

  // State rather than an event: the last one is kept and given to whoever subscribes
  // next. An event says something happened and is gone; state says what is true now,
  // and a monitor that connects after it was said still has to be able to learn it.
  //
  // Defaulted rather than pure, because a bus that cannot retain is not a broken bus -
  // it is one where this degrades to an ordinary publish, and an existing driver keeps
  // compiling and keeps working.
  virtual void publish_state(std::string event_name, std::string message) { publish(std::move(event_name), std::move(message)); }

  // What the bus should say on this node's behalf if it stops saying anything at all.
  // A node that dies does not get to publish its own obituary, so the broker is asked
  // in advance to publish one for it (MQTT calls this the will).
  //
  // Set before connect() or not at all: a broker takes this when the session opens and
  // never afterwards. Defaulted to nothing, because a bus with no such mechanism has
  // nothing useful to do with it.
  virtual void will_set(std::string event_name, std::string message) {
    (void)event_name;
    (void)message;
  }

  // The subscription handle comes back through the handler rather than by return,
  // because a broker has to be asked before the subscription exists. Unsubscribing
  // needs the handle, so a caller that intends to unsubscribe keeps it.
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
