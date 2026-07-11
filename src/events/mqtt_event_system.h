//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/mqtt5.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "event_system.h"

namespace athenasip::events {

namespace asio = boost::asio;
namespace mqtt = boost::mqtt5;

class MQTTEventSystem final : public EventSystem, public std::enable_shared_from_this<MQTTEventSystem> {
 public:
  explicit MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::string prefix = "athenasip/", std::string broker_host = "127.0.0.1", std::uint16_t broker_port = 1883,
                           std::string client_id = "athenasip-events", std::string username = {}, std::string password = {},
                           std::uint16_t keep_alive_seconds = 30);
  MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~MQTTEventSystem() override;

  std::string get_driver_name() const override;

  MQTTEventSystem(const MQTTEventSystem&) = delete;
  MQTTEventSystem& operator=(const MQTTEventSystem&) = delete;
  MQTTEventSystem(MQTTEventSystem&&) = delete;
  MQTTEventSystem& operator=(MQTTEventSystem&&) = delete;

  bool connect() override;
  void connect(EventSystem::CallbackCompleteFn callback) override;

  bool close() override;
  void close(EventSystem::CallbackCompleteFn callback) override;

  void publish(std::string event_name, std::string message, EventSystem::CallbackCompleteFn callback) override;

  std::shared_ptr<Subscription> subscribe(std::string event_name, Subscription::EventCallbackFn event_callback,
                                          EventSystem::CallbackCompleteFn callback) override;

  void unsubscribe(std::shared_ptr<Subscription> subscription, EventSystem::CallbackCompleteFn callback) override;
  void unsubscribe_all(EventSystem::CallbackCompleteFn callback) override;

 private:
  using MQTTClient = mqtt::mqtt_client<asio::ip::tcp::socket>;
  using MQTTStrand = asio::strand<asio::io_context::executor_type>;
  using MQTTWorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

  std::shared_ptr<athenasip::loggers::Logger> _logger;
  asio::io_context& _callback_io_context;

  std::string _broker_host;
  std::uint16_t _broker_port;
  std::string _client_id;
  std::string _username;
  std::string _password;
  std::uint16_t _keep_alive_seconds;
  std::string _prefix;

  asio::io_context _mqtt_io_context;
  MQTTStrand _mqtt_strand;
  std::optional<MQTTWorkGuard> _mqtt_work_guard;
  MQTTClient _client;

  std::atomic_bool _connected{false};
  std::thread _mqtt_thread;

  mutable std::mutex _subscriptions_mutex;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscription>>> _subscriptions;

  // Accessed only on _mqtt_strand while the MQTT event loop is running.
  std::unordered_set<std::string> _broker_subscribed_events;
  bool _receive_active = false;

  void apply_url(std::shared_ptr<types::URL> url);
  void configure_client();
  void run_mqtt_io_context();
  void close_without_callback() noexcept;

  void post_complete(EventSystem::CallbackCompleteFn callback, bool ok);
  void clear_local_subscriptions();

  [[nodiscard]] std::vector<std::string> current_subscription_events() const;
  [[nodiscard]] std::unordered_set<std::shared_ptr<Subscription>> collect_matching_subscriptions(std::string event_name) const;

  void subscribe_events_on_mqtt(std::vector<std::string> event_names, EventSystem::CallbackCompleteFn callback);
  void ensure_receive_loop();
  void receive_next();
  void dispatch_event(std::string event_name, std::string message);
};

}  // namespace athenasip::events
