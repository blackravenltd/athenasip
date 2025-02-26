//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "internal_event_system.h"

#include <boost/asio/post.hpp>

namespace athenasip::events {

InternalEventSystem::InternalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger)
    : _logger(std::make_shared<loggers::LoggerScoped>("internal_event_system", logger)), _io_context(detail::getGlobalIOContext()) {}

InternalEventSystem::~InternalEventSystem() {}

void InternalEventSystem::start(std::function<void(bool)> callback) {
  auto self = shared_from_this();
  _logger->info("Started");
  if (callback) {
    boost::asio::post(_io_context, [self, callback]() { callback(true); });
  }
}

void InternalEventSystem::stop(std::function<void(bool)> callback) {
  auto self = shared_from_this();
  // Kill all subscriptions.
  unsubscribe_all(nullptr);
  if (callback) {
    boost::asio::post(_io_context, [self, callback]() { callback(true); });
  }
  _logger->info("Stopped");
}

void InternalEventSystem::publish(std::string event_name, std::string message, std::function<void(bool)> callback) {
  auto self = shared_from_this();
  _logger->debug("Publishing event: " + event_name + " with message: " + message);

  // Copy matching subscriptions under lock.
  std::unordered_set<std::shared_ptr<Subscription>> to_publish;
  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    auto it = _subscriptions.find(event_name);
    if (it != _subscriptions.end()) {
      to_publish = it->second;
    }
  }

  // Post each active subscription callback asynchronously.
  for (const auto& sub : to_publish) {
    boost::asio::post(_io_context, [this, self, event_name, message, sub]() {
      _logger->debug("Invoking subscription callback for event: " + event_name);
      sub->callback(event_name, message);
    });
  }

  if (callback) {
    // Post the publish callback.
    boost::asio::post(_io_context, [this, self, event_name, callback]() {
      _logger->debug("Invoking publish callback for event: " + event_name);
      callback(true);
    });
  }
}

std::shared_ptr<Subscription> InternalEventSystem::subscribe(std::string event_name,
                                                             std::function<void(std::string event_name, std::string message)> event_callback,
                                                             std::function<void(bool)> callback) {
  auto self = shared_from_this();
  _logger->debug("Subscribing to event: " + event_name);
  auto sub = std::make_shared<Subscription>(event_name, event_callback);
  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    _subscriptions[event_name].insert(sub);
  }
  if (callback) {
    boost::asio::post(_io_context, [this, self, callback]() {
      _logger->debug("Invoking subscribe callback");
      callback(true);
    });
  }
  return sub;
}

void InternalEventSystem::unsubscribe(std::shared_ptr<Subscription> subscription, std::function<void(bool)> callback) {
  auto self = shared_from_this();
  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    auto it = _subscriptions.find(subscription->event_name);
    if (it != _subscriptions.end()) {
      it->second.erase(subscription);
    }
  }
  if (callback) {
    boost::asio::post(_io_context, [this, self, callback]() {
      _logger->debug("Invoking unsubscribe callback");
      callback(true);
    });
  }
}

void InternalEventSystem::unsubscribe_all(std::function<void(bool)> callback) {
  auto self = shared_from_this();
  _logger->debug("Unsubscribing all subscriptions");
  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    _subscriptions.clear();
  }
  if (callback) {
    boost::asio::post(_io_context, [this, self, callback]() {
      _logger->debug("Invoking unsubscribe_all callback");
      callback(true);
    });
  }
}

}  // namespace athenasip::events
