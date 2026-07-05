//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "local_event_system.h"

#include <boost/asio/post.hpp>
#include <exception>
#include <utility>

namespace athenasip::events {

LocalEventSystem::LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger)
    : _logger(std::make_shared<loggers::LoggerScoped>("internal_event_system", std::move(logger))), _io_context(detail::getGlobalIOContext()) {}

LocalEventSystem::LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url) : LocalEventSystem(std::move(logger)) {
  (void)url;
}

LocalEventSystem::~LocalEventSystem() {}

bool LocalEventSystem::connect() {
  bool expected = false;
  if (!_connected.compare_exchange_strong(expected, true)) {
    return true;
  }

  _logger->info("Connected");
  return true;
}

void LocalEventSystem::connect(std::function<void(bool)> callback) {
  post_complete(std::move(callback), connect());
}

bool LocalEventSystem::close() {
  bool expected = true;
  if (!_connected.compare_exchange_strong(expected, false)) {
    return true;
  }

  _logger->info("Closed");
  return true;
}

void LocalEventSystem::close(std::function<void(bool)> callback) {
  post_complete(std::move(callback), close());
}

void LocalEventSystem::publish(std::string event_name, std::string message, std::function<void(bool)> callback) {
  auto self = shared_from_this();

  if (!TopicFilter::is_valid_topic(event_name)) {
    _logger->error("Publish rejected for invalid event topic: " + event_name);
    post_complete(std::move(callback), false);
    return;
  }

  if (!_connected.load()) {
    _logger->error("Publish rejected while closed: " + event_name);
    post_complete(std::move(callback), false);
    return;
  }

  _logger->debug("Publishing event: " + event_name + " with message: " + message);

  const auto to_publish = collect_matching_subscriptions(event_name);

  for (const auto& sub : to_publish) {
    boost::asio::post(_io_context, [this, self, event_name, message, sub]() {
      try {
        _logger->debug("Invoking subscription callback for event: " + event_name + " matched by: " + sub->event_name);
        sub->callback(event_name, message);
      } catch (const std::exception& e) {
        _logger->error(std::string("subscription callback failed: ") + e.what());
      } catch (...) {
        _logger->error("subscription callback failed: unknown exception");
      }
    });
  }

  post_complete(std::move(callback), true);
}

std::shared_ptr<Subscription> LocalEventSystem::subscribe(std::string event_name,
                                                          std::function<void(std::string event_name, std::string message)> event_callback,
                                                          std::function<void(bool)> callback) {
  auto self = shared_from_this();

  if (!TopicFilter::is_valid_filter(event_name)) {
    _logger->error("Subscribe rejected for invalid event filter: " + event_name);
    post_complete(std::move(callback), false);
    return nullptr;
  }

  _logger->debug("Subscribing to event: " + event_name);
  auto sub = std::make_shared<Subscription>(std::move(event_name), std::move(event_callback));

  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    _subscriptions[sub->event_name].insert(sub);
  }

  post_complete(std::move(callback), true);
  return sub;
}

void LocalEventSystem::unsubscribe(std::shared_ptr<Subscription> subscription, std::function<void(bool)> callback) {
  auto self = shared_from_this();

  if (!subscription) {
    post_complete(std::move(callback), false);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    auto it = _subscriptions.find(subscription->event_name);
    if (it != _subscriptions.end()) {
      it->second.erase(subscription);
      if (it->second.empty()) {
        _subscriptions.erase(it);
      }
    }
  }

  post_complete(std::move(callback), true);
}

void LocalEventSystem::unsubscribe_all(std::function<void(bool)> callback) {
  auto self = shared_from_this();
  _logger->debug("Unsubscribing all subscriptions");

  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    _subscriptions.clear();
  }

  post_complete(std::move(callback), true);
}

void LocalEventSystem::post_complete(std::function<void(bool)> callback, bool ok) {
  if (!callback) {
    return;
  }

  auto self = shared_from_this();
  boost::asio::post(_io_context, [self, callback = std::move(callback), ok]() mutable { callback(ok); });
}

std::unordered_set<std::shared_ptr<Subscription>> LocalEventSystem::collect_matching_subscriptions(const std::string& event_name) const {
  std::unordered_set<std::shared_ptr<Subscription>> to_publish;

  std::lock_guard<std::mutex> lock(_subscriptions_mutex);

  auto exact = _subscriptions.find(event_name);
  if (exact != _subscriptions.end()) {
    to_publish.insert(exact->second.begin(), exact->second.end());
  }

  for (const auto& [filter, subscribers] : _subscriptions) {
    if (filter == event_name || !TopicFilter::is_filter(filter)) {
      continue;
    }

    if (!TopicFilter::matches(filter, event_name)) {
      continue;
    }

    to_publish.insert(subscribers.begin(), subscribers.end());
  }

  return to_publish;
}

}  // namespace athenasip::events
