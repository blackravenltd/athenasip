#pragma once
//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "event_system.h"

namespace athenasip::events {

class InternalEventSystem : public EventSystem, public std::enable_shared_from_this<InternalEventSystem> {
 public:
  explicit InternalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger);
  virtual ~InternalEventSystem();

  // Starts the event system.
  void start(std::function<void(bool)> callback) override;

  // Stops the event system (unsubscribes all subscriptions).
  void stop(std::function<void(bool)> callback) override;

  // Publishes an event with the given name and message.
  void publish(std::string event_name, std::string message, std::function<void(bool)> callback) override;

  // Subscribes to an event; returns a subscription handle.
  std::shared_ptr<Subscription> subscribe(std::string event_name, std::function<void(std::string event_name, std::string message)> event_callback,
                                          std::function<void(bool)> callback) override;

  // Unsubscribes the given subscription.
  void unsubscribe(std::shared_ptr<Subscription> subscription, std::function<void(bool)> callback) override;

  // Unsubscribes all subscriptions.
  void unsubscribe_all(std::function<void(bool)> callback) override;

 private:
  std::shared_ptr<athenasip::loggers::Logger> _logger;
  boost::asio::io_context& _io_context;

  std::mutex _subscriptions_mutex;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscription>>> _subscriptions;
};

}  // namespace athenasip::events
