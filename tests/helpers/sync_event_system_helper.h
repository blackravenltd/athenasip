//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <future>
#include <memory>
#include <string>
#include <utility>

#include "events/event_system.h"
#include "global_io_context.h"
#include "plugins/plugin.h"

// A blocking view of an event system, for tests only (see SyncDatastore). The fire-and-forget publish is
// omitted: test it through what the consumer saw.
class SyncEventSystem {
 public:
  explicit SyncEventSystem(std::shared_ptr<athenasip::events::EventSystem> events) : _events(std::move(events)) {}

  // Synchronous in the contract.
  std::string name() const { return _events->name(); }
  std::string version() const { return _events->version(); }
  bool is_connected() const { return _events->is_connected(); }
  void close() { _events->close(); }

  std::shared_ptr<athenasip::events::EventSystem> driver() const { return _events; }

  bool connect() {
    return _status([this](auto on, auto handler) { _events->connect(std::move(on), std::move(handler)); });
  }

  bool publish(const std::string& event_name, const std::string& message) {
    return _status([this, event_name, message](auto on, auto handler) { _events->publish(std::move(on), event_name, message, std::move(handler)); });
  }

  std::shared_ptr<athenasip::events::Subscription> subscribe(const std::string& event_name, athenasip::events::Subscription::EventCallbackFn callback) {
    std::promise<athenasip::plugins::Result<std::shared_ptr<athenasip::events::Subscription>>> promise;
    auto future = promise.get_future();

    _events->subscribe(
        _executor(), event_name, std::move(callback),
        [&promise](athenasip::plugins::Result<std::shared_ptr<athenasip::events::Subscription>> result) { promise.set_value(std::move(result)); });

    auto result = future.get();
    _last_error = result.error;
    return std::move(result.value);
  }

  bool unsubscribe(std::shared_ptr<athenasip::events::Subscription> subscription) {
    return _status([this, subscription](auto on, auto handler) { _events->unsubscribe(std::move(on), subscription, std::move(handler)); });
  }

  bool unsubscribe_all() {
    return _status([this](auto on, auto handler) { _events->unsubscribe_all(std::move(on), std::move(handler)); });
  }

  // The failure the last call reported.
  const std::string& last_error() const { return _last_error; }

 private:
  template <typename Start>
  bool _status(Start start) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    start(_executor(), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    const auto status = future.get();
    _last_error = status.error;
    return status.ok;
  }

  static athenasip::plugins::Executor _executor() { return athenasip::detail::get_global_io_context().get_executor(); }

  std::shared_ptr<athenasip::events::EventSystem> _events;
  std::string _last_error;
};
