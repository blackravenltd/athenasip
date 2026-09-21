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
    : _logger(std::make_shared<loggers::LoggerScoped>("internal_event_system", std::move(logger))), _io_context(detail::get_global_io_context()) {}

LocalEventSystem::LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url) : LocalEventSystem(std::move(logger)) {
  (void)url;
}

LocalEventSystem::~LocalEventSystem() {}

std::string LocalEventSystem::name() const { return "local"; }

std::string LocalEventSystem::version() const { return "0.0.1"; }

void LocalEventSystem::connect(plugins::Executor on, plugins::StatusHandler handler) {
  bool expected = false;
  if (_connected.compare_exchange_strong(expected, true)) {
    _logger->info("Connected");
  }

  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

void LocalEventSystem::close() {
  bool expected = true;
  if (!_connected.compare_exchange_strong(expected, false)) {
    return;
  }

  _logger->info("Closed");
}

bool LocalEventSystem::is_connected() const { return _connected.load(); }

void LocalEventSystem::publish(std::string event_name, std::string message) {
  const auto status = deliver(event_name, message);
  if (!status.ok) {
    _logger->error(status.error);
  }
}

void LocalEventSystem::publish(plugins::Executor on, std::string event_name, std::string message, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), deliver(event_name, message));
}

plugins::Status LocalEventSystem::deliver(const std::string& event_name, const std::string& message) {
  auto self = shared_from_this();

  if (!TopicFilter::is_valid_topic(event_name)) {
    return plugins::Status::failure("Publish rejected for invalid event topic: " + event_name);
  }

  if (!_connected.load()) {
    return plugins::Status::failure("Publish rejected while closed: " + event_name);
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

  return plugins::Status::success();
}

void LocalEventSystem::subscribe(plugins::Executor on, std::string event_name, Subscription::EventCallbackFn event_callback,
                                 plugins::Handler<std::shared_ptr<Subscription>> handler) {
  using SubscriptionResult = plugins::Result<std::shared_ptr<Subscription>>;

  if (!TopicFilter::is_valid_filter(event_name)) {
    _logger->error("Subscribe rejected for invalid event filter: " + event_name);
    _complete(std::move(on), std::move(handler), SubscriptionResult::failure("Subscribe rejected for invalid event filter: " + event_name));
    return;
  }

  _logger->debug("Subscribing to event: " + event_name);

  _complete(std::move(on), std::move(handler), SubscriptionResult::success(add_subscription(event_name, std::move(event_callback))));
}

std::shared_ptr<Subscription> LocalEventSystem::add_subscription(const std::string& event_name, Subscription::EventCallbackFn event_callback) {
  auto sub = std::make_shared<Subscription>(event_name, std::move(event_callback));

  std::lock_guard<std::mutex> lock(_subscriptions_mutex);
  _subscriptions[sub->event_name].insert(sub);

  return sub;
}

void LocalEventSystem::unsubscribe(plugins::Executor on, std::shared_ptr<Subscription> subscription, plugins::StatusHandler handler) {
  if (!subscription) {
    _complete(std::move(on), std::move(handler), plugins::Status::failure("unsubscribe: no subscription"));
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

  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

void LocalEventSystem::unsubscribe_all(plugins::Executor on, plugins::StatusHandler handler) {
  _logger->debug("Unsubscribing all subscriptions");

  {
    std::lock_guard<std::mutex> lock(_subscriptions_mutex);
    _subscriptions.clear();
  }

  _complete(std::move(on), std::move(handler), plugins::Status::success());
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
