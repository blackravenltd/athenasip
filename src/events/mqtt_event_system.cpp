//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mqtt_event_system.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <cstddef>
#include <exception>
#include <utility>

namespace athenasip::events {

MQTTEventSystem::MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::string broker_host, std::uint16_t broker_port, std::string client_id,
                                 std::string username, std::string password, std::uint16_t keep_alive_seconds)
    : _logger(std::make_shared<loggers::LoggerScoped>("mqtt_event_system", logger)),
      _callback_io_context(detail::getGlobalIOContext()),
      _broker_host(std::move(broker_host)),
      _broker_port(broker_port),
      _client_id(std::move(client_id)),
      _username(std::move(username)),
      _password(std::move(password)),
      _keep_alive_seconds(keep_alive_seconds),
      _mqtt_strand(boost::asio::make_strand(_mqtt_io_context)),
      _client(_mqtt_strand) {}

MQTTEventSystem::~MQTTEventSystem() { stop_without_callback(); }

void MQTTEventSystem::start(EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  bool expected = false;
  if (!_running.compare_exchange_strong(expected, true)) {
    post_complete(std::move(callback), true);
    return;
  }

  _mqtt_io_context.restart();
  configure_client();

  _logger->info("Starting MQTT event system: " + _broker_host + ":" + std::to_string(_broker_port));

  boost::asio::dispatch(_mqtt_strand, [this, self]() {
    _client.async_run([this, self](mqtt::error_code ec) {
      if (!_running.load()) {
        return;
      }

      if (ec) {
        _logger->error("MQTT client stopped: " + ec.message());
      } else {
        _logger->info("MQTT client stopped");
      }
    });

    subscribe_events_on_mqtt(current_subscription_events(), nullptr);
  });

  try {
    _mqtt_thread = std::thread([this]() { run_mqtt_io_context(); });
  } catch (const std::exception& e) {
    _running.store(false);
    _logger->error(std::string("Failed to start MQTT event system thread: ") + e.what());
    post_complete(std::move(callback), false);
    return;
  } catch (...) {
    _running.store(false);
    _logger->error("Failed to start MQTT event system thread: unknown exception");
    post_complete(std::move(callback), false);
    return;
  }

  _logger->debug("Started");
  post_complete(std::move(callback), true);
}

void MQTTEventSystem::stop(EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  bool expected = true;
  if (!_running.compare_exchange_strong(expected, false)) {
    clear_local_subscriptions();
    post_complete(std::move(callback), true);
    return;
  }

  _logger->debug("Stopping");
  clear_local_subscriptions();

  boost::asio::post(_mqtt_strand, [this, self]() { _client.cancel(); });

  if (_mqtt_thread.joinable() && _mqtt_thread.get_id() != std::this_thread::get_id()) {
    _mqtt_thread.join();
  }

  _broker_subscribed_events.clear();
  _receive_active = false;

  _logger->info("Stopped");
  post_complete(std::move(callback), true);
}

void MQTTEventSystem::publish(std::string event_name, std::string message, EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  if (!_running.load()) {
    _logger->error("MQTT publish rejected while stopped: " + event_name);
    post_complete(std::move(callback), false);
    return;
  }

  _logger->debug("Publishing MQTT event: " + event_name + " with message: " + message);

  boost::asio::dispatch(_mqtt_strand, [this, self, event_name = std::move(event_name), message = std::move(message), callback = std::move(callback)]() mutable {
    if (!_running.load()) {
      post_complete(std::move(callback), false);
      return;
    }

    _client.async_publish<mqtt::qos_e::at_most_once>(std::move(event_name), std::move(message), mqtt::retain_e::no, mqtt::publish_props{},
                                                     [this, self, callback = std::move(callback)](mqtt::error_code ec) mutable {
                                                       if (ec) {
                                                         _logger->error("MQTT publish failed: " + ec.message());
                                                         post_complete(std::move(callback), false);
                                                         return;
                                                       }

                                                       post_complete(std::move(callback), true);
                                                     });
  });
}

std::shared_ptr<Subscription> MQTTEventSystem::subscribe(std::string event_name, Subscription::EventCallbackFn event_callback,
                                                         EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  _logger->debug("Subscribing to MQTT event: " + event_name);
  auto sub = std::make_shared<Subscription>(event_name, std::move(event_callback));

  bool first_local_subscription = false;
  {
    std::scoped_lock lock(_subscriptions_mutex);
    auto& subscribers = _subscriptions[sub->event_name];
    first_local_subscription = subscribers.empty();
    subscribers.insert(sub);
  }

  if (!_running.load() || !first_local_subscription) {
    post_complete(std::move(callback), true);
    return sub;
  }

  boost::asio::dispatch(_mqtt_strand, [this, self, event_name = sub->event_name, callback = std::move(callback)]() mutable {
    subscribe_events_on_mqtt(std::vector<std::string>{std::move(event_name)}, std::move(callback));
  });

  return sub;
}

void MQTTEventSystem::unsubscribe(std::shared_ptr<Subscription> subscription, EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  if (!subscription) {
    post_complete(std::move(callback), false);
    return;
  }

  {
    std::scoped_lock lock(_subscriptions_mutex);
    auto it = _subscriptions.find(subscription->event_name);
    if (it != _subscriptions.end()) {
      it->second.erase(subscription);
      if (it->second.empty()) {
        _subscriptions.erase(it);
      }
    }
  }

  // Deliberately do not UNSUBSCRIBE at the broker. Keeping the broker-side
  // subscription avoids races with a new local subscription to the same event.
  // Messages for events with no local subscribers are simply dropped.
  post_complete(std::move(callback), true);
}

void MQTTEventSystem::unsubscribe_all(EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();
  _logger->debug("Unsubscribing all MQTT event subscriptions");
  clear_local_subscriptions();
  post_complete(std::move(callback), true);
}

void MQTTEventSystem::configure_client() {
  _client.brokers(_broker_host, _broker_port).credentials(_client_id).keep_alive(_keep_alive_seconds);

  if (!_username.empty()) {
    _client.credentials(_client_id, _username, _password);
  }
}

void MQTTEventSystem::run_mqtt_io_context() {
  try {
    _mqtt_io_context.run();
  } catch (const std::exception& e) {
    _logger->error(std::string("MQTT event system io_context failed: ") + e.what());
  } catch (...) {
    _logger->error("MQTT event system io_context failed: unknown exception");
  }
}

void MQTTEventSystem::stop_without_callback() noexcept {
  bool expected = true;
  if (!_running.compare_exchange_strong(expected, false)) {
    return;
  }

  try {
    clear_local_subscriptions();
    boost::asio::post(_mqtt_strand, [this]() { _client.cancel(); });

    if (_mqtt_thread.joinable() && _mqtt_thread.get_id() != std::this_thread::get_id()) {
      _mqtt_thread.join();
    }

    _broker_subscribed_events.clear();
    _receive_active = false;
  } catch (...) {
    // Destructors must not throw.
  }
}

void MQTTEventSystem::post_complete(EventSystem::CallbackCompleteFn callback, bool ok) {
  if (!callback) {
    return;
  }

  auto self = shared_from_this();
  boost::asio::post(_callback_io_context, [self, callback = std::move(callback), ok]() mutable { callback(ok); });
}

void MQTTEventSystem::clear_local_subscriptions() {
  std::scoped_lock lock(_subscriptions_mutex);
  _subscriptions.clear();
}

std::vector<std::string> MQTTEventSystem::current_subscription_events() const {
  std::vector<std::string> events;

  std::scoped_lock lock(_subscriptions_mutex);
  events.reserve(_subscriptions.size());

  for (const auto& [event_name, subscribers] : _subscriptions) {
    if (!subscribers.empty()) {
      events.push_back(event_name);
    }
  }

  return events;
}

void MQTTEventSystem::subscribe_events_on_mqtt(std::vector<std::string> event_names, EventSystem::CallbackCompleteFn callback) {
  if (!_running.load()) {
    post_complete(std::move(callback), false);
    return;
  }

  std::vector<std::string> to_subscribe;
  to_subscribe.reserve(event_names.size());

  for (auto& event_name : event_names) {
    if (_broker_subscribed_events.insert(event_name).second) {
      to_subscribe.push_back(std::move(event_name));
    }
  }

  if (to_subscribe.empty()) {
    ensure_receive_loop();
    post_complete(std::move(callback), true);
    return;
  }

  std::vector<mqtt::subscribe_topic> topics;
  topics.reserve(to_subscribe.size());

  for (const auto& event_name : to_subscribe) {
    _logger->info("MQTT subscribing to event: " + event_name);
    topics.push_back(mqtt::subscribe_topic{event_name, mqtt::subscribe_options{mqtt::qos_e::at_most_once, mqtt::no_local_e::no,
                                                                               mqtt::retain_as_published_e::retain, mqtt::retain_handling_e::send}});
  }

  auto self = shared_from_this();
  _client.async_subscribe(std::move(topics), mqtt::subscribe_props{},
                          [this, self, to_subscribe = std::move(to_subscribe), callback = std::move(callback)](
                              mqtt::error_code ec, std::vector<mqtt::reason_code> reasons, mqtt::suback_props props) mutable {
                            (void)props;

                            if (!_running.load()) {
                              post_complete(std::move(callback), false);
                              return;
                            }

                            if (ec) {
                              for (const auto& event_name : to_subscribe) {
                                _broker_subscribed_events.erase(event_name);
                              }
                              _logger->error("MQTT subscribe failed: " + ec.message());
                              post_complete(std::move(callback), false);
                              return;
                            }

                            if (reasons.empty()) {
                              for (const auto& event_name : to_subscribe) {
                                _broker_subscribed_events.erase(event_name);
                              }
                              _logger->error("MQTT subscribe failed: empty SUBACK reason list");
                              post_complete(std::move(callback), false);
                              return;
                            }

                            bool all_accepted = true;
                            bool any_accepted = false;

                            for (std::size_t i = 0; i < to_subscribe.size(); ++i) {
                              const auto& event_name = to_subscribe[i];

                              if (i >= reasons.size()) {
                                all_accepted = false;
                                _broker_subscribed_events.erase(event_name);
                                _logger->error("MQTT subscribe rejected: event=" + event_name + ", missing SUBACK reason");
                                continue;
                              }

                              const auto& reason = reasons[i];
                              const std::uint8_t code = reason.value();

                              if (code >= 0x80) {
                                all_accepted = false;
                                _broker_subscribed_events.erase(event_name);
                                _logger->error("MQTT subscribe rejected: event=" + event_name + ", reason=" + std::to_string(static_cast<unsigned>(code)) +
                                               " (" + reason.message() + ")");
                              } else {
                                any_accepted = true;
                                _logger->info("MQTT subscribe accepted: event=" + event_name + ", reason=" + std::to_string(static_cast<unsigned>(code)) +
                                              " (" + reason.message() + ")");
                              }
                            }

                            if (any_accepted) {
                              ensure_receive_loop();
                            }

                            post_complete(std::move(callback), all_accepted && any_accepted);
                          });
}

void MQTTEventSystem::ensure_receive_loop() {
  if (!_running.load() || _receive_active || _broker_subscribed_events.empty()) {
    return;
  }

  _receive_active = true;
  receive_next();
}

void MQTTEventSystem::receive_next() {
  auto self = shared_from_this();

  _client.async_receive([this, self](mqtt::error_code ec, std::string topic, std::string payload, mqtt::publish_props props) {
    (void)props;
    _receive_active = false;

    if (!_running.load()) {
      return;
    }

    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        return;
      }

      if (ec == mqtt::client::error::session_expired) {
        _logger->error("MQTT receive failed: session expired; re-subscribing");
      } else {
        _logger->error("MQTT receive failed: " + ec.message() + "; re-subscribing");
      }

      _broker_subscribed_events.clear();
      subscribe_events_on_mqtt(current_subscription_events(), nullptr);
      return;
    }

    _logger->debug("MQTT received event: " + topic + " with message: " + payload);

    dispatch_event(std::move(topic), std::move(payload));
    ensure_receive_loop();
  });
}

void MQTTEventSystem::dispatch_event(std::string event_name, std::string message) {
  std::unordered_set<std::shared_ptr<Subscription>> to_publish;
  {
    std::scoped_lock lock(_subscriptions_mutex);
    auto it = _subscriptions.find(event_name);
    if (it != _subscriptions.end()) {
      to_publish = it->second;
    }
  }

  for (const auto& sub : to_publish) {
    auto self = shared_from_this();
    boost::asio::post(_callback_io_context, [this, self, event_name, message, sub]() {
      try {
        _logger->debug("Invoking MQTT event subscription callback for event: " + event_name);
        sub->callback(event_name, message);
      } catch (const std::exception& e) {
        _logger->error(std::string("MQTT event subscription callback failed: ") + e.what());
      } catch (...) {
        _logger->error("MQTT event subscription callback failed: unknown exception");
      }
    });
  }
}

}  // namespace athenasip::events
