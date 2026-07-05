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
#include "../types/url.h"
#include "event_system.h"

namespace athenasip::events {

class LocalEventSystem : public EventSystem, public std::enable_shared_from_this<LocalEventSystem> {
 public:
  explicit LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger);
  LocalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~LocalEventSystem() override;

  bool connect() override;
  void connect(std::function<void(bool)> callback) override;

  bool close() override;
  void close(std::function<void(bool)> callback) override;

  void publish(std::string event_name, std::string message, std::function<void(bool)> callback) override;

  std::shared_ptr<Subscription> subscribe(std::string event_name, std::function<void(std::string event_name, std::string message)> event_callback,
                                          std::function<void(bool)> callback) override;

  void unsubscribe(std::shared_ptr<Subscription> subscription, std::function<void(bool)> callback) override;
  void unsubscribe_all(std::function<void(bool)> callback) override;

 private:
  void post_complete(std::function<void(bool)> callback, bool ok);
  std::unordered_set<std::shared_ptr<Subscription>> collect_matching_subscriptions(const std::string& event_name) const;

  std::shared_ptr<athenasip::loggers::Logger> _logger;
  boost::asio::io_context& _io_context;

  std::atomic_bool _connected{false};

  mutable std::mutex _subscriptions_mutex;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscription>>> _subscriptions;
};

}  // namespace athenasip::events
