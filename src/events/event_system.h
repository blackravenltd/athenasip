//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace athenasip::events {

class Subscription {
 public:
  using EventCallbackFn = std::function<void(std::string, std::string)>;

  std::string event_name;
  EventCallbackFn callback;

  Subscription(std::string _event_name, EventCallbackFn _callback) : event_name(_event_name), callback(_callback) {}
};

class EventSystem {
 public:
  using CallbackCompleteFn = std::function<void(bool)>;

  virtual ~EventSystem() = default;

  virtual void start(CallbackCompleteFn callback) = 0;
  virtual void start() { start(nullptr); }
  virtual void stop(CallbackCompleteFn callback) = 0;
  virtual void stop() { stop(nullptr); }

  virtual void publish(std::string event_name, std::string message, CallbackCompleteFn callback) = 0;
  virtual void publish(std::string event_name, std::string message) { publish(event_name, message, nullptr); }
  virtual std::shared_ptr<Subscription> subscribe(std::string event_name, Subscription::EventCallbackFn event_callback, CallbackCompleteFn callback) = 0;
  virtual std::shared_ptr<Subscription> subscribe(std::string event_name, Subscription::EventCallbackFn event_callback) {
    return subscribe(event_name, event_callback, nullptr);
  }
  virtual void unsubscribe(std::shared_ptr<Subscription> subscription, CallbackCompleteFn callback) = 0;
  virtual void unsubscribe(std::shared_ptr<Subscription> subscription) { unsubscribe(subscription, nullptr); }
  virtual void unsubscribe_all(CallbackCompleteFn callback) = 0;
  virtual void unsubscribe_all() { unsubscribe_all(nullptr); }
};

}  // namespace athenasip::events