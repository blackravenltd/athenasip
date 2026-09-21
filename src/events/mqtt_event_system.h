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
#include <functional>
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
#include "../plugins/plugin.h"
#include "../types/url.h"
#include "event_system.h"

namespace athenasip::events {

namespace asio = boost::asio;
namespace mqtt = boost::mqtt5;

class MQTTEventSystem final : public EventSystem, public std::enable_shared_from_this<MQTTEventSystem> {
 public:
  explicit MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::string prefix = "athenasip/", std::string broker_host = "127.0.0.1",
                           std::uint16_t broker_port = 1883, std::string client_id = "athenasip-events", std::string username = {}, std::string password = {},
                           std::uint16_t keep_alive_seconds = 30);
  MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~MQTTEventSystem() override;

  std::string name() const override;
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;

  MQTTEventSystem(const MQTTEventSystem&) = delete;
  MQTTEventSystem& operator=(const MQTTEventSystem&) = delete;
  MQTTEventSystem(MQTTEventSystem&&) = delete;
  MQTTEventSystem& operator=(MQTTEventSystem&&) = delete;

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
  // A handler already bound to the executor it answers on. The broker work happens on
  // the MQTT strand, several frames from the caller, and carrying one callable there is
  // simpler than carrying an executor and a handler side by side. Null means the caller
  // did not ask, which is the fire-and-forget publish.
  using Completion = std::function<void(plugins::Status)>;

  Completion bind_completion(plugins::Executor on, plugins::StatusHandler handler);
  static void finish(const Completion& completion, plugins::Status status);

  void publish_event(std::string event_name, std::string message, Completion completion);

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

  void clear_local_subscriptions();
  void remove_subscription(const std::shared_ptr<Subscription>& subscription);

  [[nodiscard]] std::vector<std::string> current_subscription_events() const;
  [[nodiscard]] std::unordered_set<std::shared_ptr<Subscription>> collect_matching_subscriptions(std::string event_name) const;

  void subscribe_events_on_mqtt(std::vector<std::string> event_names, Completion completion);
  void ensure_receive_loop();
  void receive_next();
  void dispatch_event(std::string event_name, std::string message);
};

}  // namespace athenasip::events
