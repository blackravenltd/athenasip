//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mqtt_event_system.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <cstddef>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace athenasip::events {
namespace {

std::uint16_t parse_port(const std::string& value, std::uint16_t default_port) {
  if (value.empty()) {
    return default_port;
  }

  const auto parsed = std::stoul(value);
  if (parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("MQTT URL port is out of range: " + value);
  }

  return static_cast<std::uint16_t>(parsed);
}

std::unordered_map<std::string, std::string> parse_query(std::string value) {
  std::unordered_map<std::string, std::string> result;

  while (!value.empty()) {
    const auto amp = value.find('&');
    auto part = amp == std::string::npos ? value : value.substr(0, amp);
    if (amp == std::string::npos) {
      value.clear();
    } else {
      value.erase(0, amp + 1);
    }

    if (part.empty()) {
      continue;
    }

    const auto eq = part.find('=');
    if (eq == std::string::npos) {
      result[std::move(part)] = {};
      continue;
    }

    auto key = part.substr(0, eq);
    part.erase(0, eq + 1);
    result[std::move(key)] = std::move(part);
  }

  return result;
}

}  // namespace

MQTTEventSystem::MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::string broker_host, std::uint16_t broker_port, std::string client_id,
                                 std::string username, std::string password, std::uint16_t keep_alive_seconds)
    : _logger(std::make_shared<loggers::LoggerScoped>("mqtt_event_system", std::move(logger))),
      _callback_io_context(detail::getGlobalIOContext()),
      _broker_host(std::move(broker_host)),
      _broker_port(broker_port),
      _client_id(std::move(client_id)),
      _username(std::move(username)),
      _password(std::move(password)),
      _keep_alive_seconds(keep_alive_seconds),
      _mqtt_strand(boost::asio::make_strand(_mqtt_io_context)),
      _client(_mqtt_strand) {}

MQTTEventSystem::MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url) : MQTTEventSystem(std::move(logger)) {
  apply_url(std::move(url));
}

MQTTEventSystem::~MQTTEventSystem() { close_without_callback(); }

std::string MQTTEventSystem::get_driver_name() const {
  return "AthenaSIP MQTT Driver v0.0.1";
}

bool MQTTEventSystem::connect() {
  auto self = shared_from_this();

  bool expected = false;
  if (!_connected.compare_exchange_strong(expected, true)) {
    return true;
  }

  _mqtt_io_context.restart();
  _mqtt_work_guard.emplace(_mqtt_io_context.get_executor());
  configure_client();

  _logger->info("Connecting: " + _broker_host + ":" + std::to_string(_broker_port));

  try {
    _mqtt_thread = std::thread([this]() { run_mqtt_io_context(); });
  } catch (const std::exception& e) {
    _mqtt_work_guard.reset();
    _connected.store(false);
    _logger->error(std::string("Failed to start MQTT thread: ") + e.what());
    return false;
  } catch (...) {
    _mqtt_work_guard.reset();
    _connected.store(false);
    _logger->error("Failed to start MQTT thread: unknown exception");
    return false;
  }

  boost::asio::dispatch(_mqtt_strand, [this, self]() {
    _client.async_run([this, self](mqtt::error_code ec) {
      if (!_connected.load()) {
        return;
      }

      _connected.store(false);
      _receive_active = false;
      _broker_subscribed_events.clear();
      _mqtt_work_guard.reset();

      if (ec == boost::asio::error::operation_aborted) {
        _logger->info("Client closed");
      } else if (ec) {
        _logger->error("Client closed: " + ec.message());
      } else {
        _logger->info("Client closed");
      }
    });

    subscribe_events_on_mqtt(current_subscription_events(), nullptr);
  });

  _logger->debug("Connected");
  return true;
}

void MQTTEventSystem::connect(EventSystem::CallbackCompleteFn callback) {
  post_complete(std::move(callback), connect());
}

bool MQTTEventSystem::close() {
  auto self = shared_from_this();

  bool expected = true;
  if (!_connected.compare_exchange_strong(expected, false)) {
    return true;
  }

  _logger->debug("Closing");

  boost::asio::post(_mqtt_strand, [this, self]() {
    _client.async_disconnect([this, self](mqtt::error_code ec) {
      if (ec && ec != boost::asio::error::operation_aborted) {
        _logger->error("MQTT disconnect failed: " + ec.message());
      }
    });
  });
  _mqtt_work_guard.reset();

  if (_mqtt_thread.joinable() && _mqtt_thread.get_id() != std::this_thread::get_id()) {
    _mqtt_thread.join();
  }

  _broker_subscribed_events.clear();
  _receive_active = false;

  _logger->info("Closed");
  return true;
}

void MQTTEventSystem::close(EventSystem::CallbackCompleteFn callback) {
  post_complete(std::move(callback), close());
}

void MQTTEventSystem::publish(std::string event_name, std::string message, EventSystem::CallbackCompleteFn callback) {
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

  boost::asio::dispatch(_mqtt_strand, [this, self, event_name = std::move(event_name), message = std::move(message), callback = std::move(callback)]() mutable {
    if (!_connected.load()) {
      post_complete(std::move(callback), false);
      return;
    }

    auto pub_callback = ([this, self, callback = std::move(callback)](mqtt::error_code ec) mutable {
      if (ec) {
        _logger->error("MQTT publish failed: " + ec.message());
        post_complete(std::move(callback), false);
        return;
      }

      post_complete(std::move(callback), true);
    });

    _client.async_publish<mqtt::qos_e::at_most_once>(std::move(event_name), std::move(message), mqtt::retain_e::no, mqtt::publish_props{},
                                                     std::move(pub_callback));
  });
}

std::shared_ptr<Subscription> MQTTEventSystem::subscribe(std::string event_name, Subscription::EventCallbackFn event_callback,
                                                         EventSystem::CallbackCompleteFn callback) {
  auto self = shared_from_this();

  if (!TopicFilter::is_valid_filter(event_name)) {
    _logger->error("Subscribe rejected for invalid event filter: " + event_name);
    post_complete(std::move(callback), false);
    return nullptr;
  }

  _logger->debug("Subscribing to event: " + event_name);
  auto sub = std::make_shared<Subscription>(std::move(event_name), std::move(event_callback));

  bool first_local_subscription = false;
  {
    std::scoped_lock lock(_subscriptions_mutex);
    auto& subscribers = _subscriptions[sub->event_name];
    first_local_subscription = subscribers.empty();
    subscribers.insert(sub);
  }

  if (!_connected.load() || !first_local_subscription) {
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
  _logger->debug("Unsubscribing all subscriptions");
  clear_local_subscriptions();
  post_complete(std::move(callback), true);
}

void MQTTEventSystem::apply_url(std::shared_ptr<types::URL> url) {
  _broker_host = "127.0.0.1";
  _broker_port = 1883;
  _client_id = "athenasip-events";
  _username.clear();
  _password.clear();
  _keep_alive_seconds = 30;

  if (!url) {
    return;
  }

  std::string value = url->to_string();
  std::string scheme = url->scheme;

  const auto scheme_pos = value.find("://");
  if (scheme_pos != std::string::npos) {
    scheme = value.substr(0, scheme_pos);
    value.erase(0, scheme_pos + 3);
  }

  if (scheme == "mqtts") {
    _broker_port = 8883;
    _logger->warn("mqtts:// selected, but MQTT TLS transport is not implemented by this backend yet");
  }

  const auto path_pos = value.find('/');
  std::string authority = path_pos == std::string::npos ? value : value.substr(0, path_pos);
  std::string path = path_pos == std::string::npos ? std::string{} : value.substr(path_pos + 1);
  std::string query;

  const auto query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    query = path.substr(query_pos + 1);
    path.erase(query_pos);
  }

  const auto authority_query_pos = authority.find('?');
  if (authority_query_pos != std::string::npos) {
    query = authority.substr(authority_query_pos + 1);
    authority.erase(authority_query_pos);
  }

  const auto auth_pos = authority.rfind('@');
  if (auth_pos != std::string::npos) {
    const auto auth = authority.substr(0, auth_pos);
    authority.erase(0, auth_pos + 1);

    const auto colon_pos = auth.find(':');
    if (colon_pos == std::string::npos) {
      _username = auth;
    } else {
      _username = auth.substr(0, colon_pos);
      _password = auth.substr(colon_pos + 1);
    }
  }

  if (!authority.empty()) {
    if (authority.front() == '[') {
      const auto close = authority.find(']');
      if (close == std::string::npos) {
        throw std::runtime_error("Invalid MQTT URL IPv6 authority: " + authority);
      }

      _broker_host = authority.substr(1, close - 1);
      if (close + 1 < authority.size()) {
        if (authority[close + 1] != ':') {
          throw std::runtime_error("Invalid MQTT URL authority: " + authority);
        }
        _broker_port = parse_port(authority.substr(close + 2), _broker_port);
      }
    } else {
      const auto colon_pos = authority.rfind(':');
      if (colon_pos == std::string::npos) {
        _broker_host = authority;
      } else {
        _broker_host = authority.substr(0, colon_pos);
        _broker_port = parse_port(authority.substr(colon_pos + 1), _broker_port);
      }
    }
  }

  const auto params = parse_query(std::move(query));
  auto it = params.find("client_id");
  if (it != params.end() && !it->second.empty()) {
    _client_id = it->second;
  }

  it = params.find("keep_alive");
  if (it == params.end()) {
    it = params.find("keepalive");
  }
  if (it != params.end() && !it->second.empty()) {
    _keep_alive_seconds = parse_port(it->second, _keep_alive_seconds);
  }

  it = params.find("username");
  if (it != params.end()) {
    _username = it->second;
  }

  it = params.find("password");
  if (it != params.end()) {
    _password = it->second;
  }

  (void)path;
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
    _logger->error(std::string("io_context failed: ") + e.what());
  } catch (...) {
    _logger->error("io_context failed: unknown exception");
  }
}

void MQTTEventSystem::close_without_callback() noexcept {
  bool expected = true;
  if (!_connected.compare_exchange_strong(expected, false)) {
    return;
  }

  try {
    boost::asio::post(_mqtt_strand, [this]() {
      _client.async_disconnect([](mqtt::error_code) {});
    });
    _mqtt_work_guard.reset();

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

std::unordered_set<std::shared_ptr<Subscription>> MQTTEventSystem::collect_matching_subscriptions(const std::string& event_name) const {
  std::unordered_set<std::shared_ptr<Subscription>> to_publish;

  std::scoped_lock lock(_subscriptions_mutex);

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

void MQTTEventSystem::subscribe_events_on_mqtt(std::vector<std::string> event_names, EventSystem::CallbackCompleteFn callback) {
  if (!_connected.load()) {
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
    _logger->info("subscribing to event: " + event_name);
    topics.push_back(mqtt::subscribe_topic{event_name, mqtt::subscribe_options{mqtt::qos_e::at_most_once, mqtt::no_local_e::no,
                                                                               mqtt::retain_as_published_e::retain, mqtt::retain_handling_e::send}});
  }

  auto self = shared_from_this();
  _client.async_subscribe(std::move(topics), mqtt::subscribe_props{},
                          [this, self, to_subscribe = std::move(to_subscribe), callback = std::move(callback)](
                              mqtt::error_code ec, std::vector<mqtt::reason_code> reasons, mqtt::suback_props props) mutable {
                            (void)props;

                            if (!_connected.load()) {
                              post_complete(std::move(callback), false);
                              return;
                            }

                            if (ec) {
                              for (const auto& event_name : to_subscribe) {
                                _broker_subscribed_events.erase(event_name);
                              }
                              _logger->error("subscribe failed: " + ec.message());
                              post_complete(std::move(callback), false);
                              return;
                            }

                            if (reasons.empty()) {
                              for (const auto& event_name : to_subscribe) {
                                _broker_subscribed_events.erase(event_name);
                              }
                              _logger->error("subscribe failed: empty SUBACK reason list");
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
                                _logger->error("subscribe rejected: event=" + event_name + ", missing SUBACK reason");
                                continue;
                              }

                              const auto& reason = reasons[i];
                              const std::uint8_t code = reason.value();

                              if (code >= 0x80) {
                                all_accepted = false;
                                _broker_subscribed_events.erase(event_name);
                                _logger->error("subscribe rejected: event=" + event_name + ", reason=" + std::to_string(static_cast<unsigned>(code)) + " (" +
                                               reason.message() + ")");
                              } else {
                                any_accepted = true;
                                _logger->info("subscribe accepted: event=" + event_name + ", reason=" + std::to_string(static_cast<unsigned>(code)) + " (" +
                                              reason.message() + ")");
                              }
                            }

                            if (any_accepted) {
                              ensure_receive_loop();
                            }

                            post_complete(std::move(callback), all_accepted && any_accepted);
                          });
}

void MQTTEventSystem::ensure_receive_loop() {
  if (!_connected.load() || _receive_active || _broker_subscribed_events.empty()) {
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

    if (!_connected.load()) {
      return;
    }

    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        return;
      }

      if (ec == mqtt::client::error::session_expired) {
        _logger->info("receive failed: session expired; re-subscribing");
      } else {
        _logger->error("receive failed: " + ec.message() + "; re-subscribing");
      }

      _broker_subscribed_events.clear();
      subscribe_events_on_mqtt(current_subscription_events(), nullptr);
      return;
    }

    _logger->debug("received event: " + topic + " with message: " + payload);

    dispatch_event(std::move(topic), std::move(payload));
    ensure_receive_loop();
  });
}

void MQTTEventSystem::dispatch_event(std::string event_name, std::string message) {
  if (!TopicFilter::is_valid_topic(event_name)) {
    _logger->error("Dropping invalid received event topic: " + event_name);
    return;
  }

  const auto to_publish = collect_matching_subscriptions(event_name);

  for (const auto& sub : to_publish) {
    auto self = shared_from_this();
    boost::asio::post(_callback_io_context, [this, self, event_name, message, sub]() {
      try {
        sub->callback(event_name, message);
      } catch (const std::exception& e) {
        _logger->error(std::string("subscription callback failed: ") + e.what());
      } catch (...) {
        _logger->error("subscription callback failed: unknown exception");
      }
    });
  }
}

}  // namespace athenasip::events
