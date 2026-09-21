//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../plugins/plugin.h"
#include "../types/url.h"
#include "event_system.h"

namespace athenasip::events {

class LocalEventSystem : public EventSystem, public std::enable_shared_from_this<LocalEventSystem> {
 public:
  explicit LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger);
  LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~LocalEventSystem() override;

  std::string name() const override;
  std::string version() const override;

  void connect(plugins::Executor on, plugins::StatusHandler handler) override;
  void close() override;
  bool is_connected() const override;

  void publish(std::string event_name, std::string message) override;
  void publish(plugins::Executor on, std::string event_name, std::string message, plugins::StatusHandler handler) override;

  void subscribe(plugins::Executor on, std::string event_name, Subscription::EventCallbackFn event_callback,
                 plugins::Handler<std::shared_ptr<Subscription>> handler) override;

  void unsubscribe(plugins::Executor on, std::shared_ptr<Subscription> subscription, plugins::StatusHandler handler) override;
  void unsubscribe_all(plugins::Executor on, plugins::StatusHandler handler) override;

 private:
  // The delivery half of a publish, shared by both forms: what it could not do is the
  // reason a caller that asked for one gets back.
  plugins::Status deliver(const std::string& event_name, const std::string& message);

  std::shared_ptr<Subscription> add_subscription(const std::string& event_name, Subscription::EventCallbackFn event_callback);
  std::unordered_set<std::shared_ptr<Subscription>> collect_matching_subscriptions(const std::string& event_name) const;

  std::shared_ptr<athenasip::loggers::Logger> _logger;
  boost::asio::io_context& _io_context;

  std::atomic_bool _connected{false};

  mutable std::mutex _subscriptions_mutex;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscription>>> _subscriptions;
};

}  // namespace athenasip::events
