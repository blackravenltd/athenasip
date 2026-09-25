//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "events/event_system.h"

// A bus that delivers nothing and remembers everything, including which publishes were
// state and what the broker was told to say if this node vanished. What a test about
// the node's own announcements needs is the record, not the delivery.
class RecordingEventSystem : public athenasip::events::EventSystem {
 public:
  using Entry = std::pair<std::string, std::string>;

  std::string name() const override { return "recording"; }
  std::string version() const override { return "0.0.1"; }

  void connect(athenasip::plugins::Executor on, athenasip::plugins::StatusHandler handler) override {
    _connected = true;
    _complete(std::move(on), std::move(handler), athenasip::plugins::Status::success());
  }

  void close() override { _connected = false; }
  bool is_connected() const override { return _connected; }

  void publish(std::string event_name, std::string message) override { _record(_published, std::move(event_name), std::move(message)); }

  void publish(athenasip::plugins::Executor on, std::string event_name, std::string message, athenasip::plugins::StatusHandler handler) override {
    _record(_published, std::move(event_name), std::move(message));
    _complete(std::move(on), std::move(handler), athenasip::plugins::Status::success());
  }

  void publish_state(std::string event_name, std::string message) override {
    _record(_states, event_name, message);
    _record(_published, std::move(event_name), std::move(message));
  }

  void will_set(std::string event_name, std::string message) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _will = Entry{std::move(event_name), std::move(message)};
  }

  void subscribe(athenasip::plugins::Executor on, std::string event_name, athenasip::events::Subscription::EventCallbackFn event_callback,
                 athenasip::plugins::Handler<std::shared_ptr<athenasip::events::Subscription>> handler) override {
    (void)event_callback;
    auto subscription = std::make_shared<athenasip::events::Subscription>(std::move(event_name), nullptr);
    _complete(std::move(on), std::move(handler),
              athenasip::plugins::Result<std::shared_ptr<athenasip::events::Subscription>>::success(std::move(subscription)));
  }

  void unsubscribe(athenasip::plugins::Executor on, std::shared_ptr<athenasip::events::Subscription> subscription,
                   athenasip::plugins::StatusHandler handler) override {
    (void)subscription;
    _complete(std::move(on), std::move(handler), athenasip::plugins::Status::success());
  }

  void unsubscribe_all(athenasip::plugins::Executor on, athenasip::plugins::StatusHandler handler) override {
    _complete(std::move(on), std::move(handler), athenasip::plugins::Status::success());
  }

  std::vector<Entry> published() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _published;
  }

  std::vector<Entry> states() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states;
  }

  std::optional<Entry> will() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _will;
  }

 private:
  void _record(std::vector<Entry>& into, std::string event_name, std::string message) {
    std::lock_guard<std::mutex> lock(_mutex);
    into.emplace_back(std::move(event_name), std::move(message));
  }

  mutable std::mutex _mutex;
  std::vector<Entry> _published;
  std::vector<Entry> _states;
  std::optional<Entry> _will;
  bool _connected = false;
};
