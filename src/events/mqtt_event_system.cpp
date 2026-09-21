//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mqtt_event_system.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "../config.h"
#include "../util.h"

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

std::string prefixed_topic(const std::string& prefix, const std::string& event_name) { return prefix + event_name; }

// A prefix is a topic level, and levels are separated by '/'. Concatenating one that
// does not end in a separator produces "athenasipnodes/sip-0001/status", which is a
// topic no filter anybody writes will match and a misconfiguration nothing complains
// about. Given with or without the separator, it means the same thing.
std::string normalise_prefix(std::string prefix) {
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  return prefix;
}

bool remove_prefix(const std::string& prefix, std::string& topic) {
  if (prefix.empty()) {
    return true;
  }

  if (!topic.starts_with(prefix)) {
    return false;
  }

  topic.erase(0, prefix.size());
  return true;
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

// Unique to this instance, because MQTT makes a repeated client identifier mean "the
// other one is stale, disconnect it" and two clients trading that back and forth is a
// bus that carries nothing. configure() replaces this with the node id when there is
// one, which is stable across restarts and unique across the cluster by definition.
std::string default_client_id() { return Util::generate_random_string("athenasip-", 12); }

}  // namespace

MQTTEventSystem::MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::string prefix, std::string broker_host, std::uint16_t broker_port,
                                 std::string client_id, std::string username, std::string password, std::uint16_t keep_alive_seconds)
    : _logger(std::make_shared<loggers::LoggerScoped>("mqtt_event_system", std::move(logger))),
      _callback_io_context(detail::get_global_io_context()),
      _prefix(std::move(prefix)),
      _broker_host(std::move(broker_host)),
      _broker_port(broker_port),
      _client_id(std::move(client_id)),
      _username(std::move(username)),
      _password(std::move(password)),
      _keep_alive_seconds(keep_alive_seconds),
      _mqtt_strand(boost::asio::make_strand(_mqtt_io_context)),
      _client(_mqtt_strand) {
  // Not in the initialiser list: members are initialised in declaration order, so a
  // flag computed there from a parameter the line above has moved from reads whatever
  // the move left behind.
  _client_id_was_given = !_client_id.empty();
  if (!_client_id_was_given) _client_id = default_client_id();
}

MQTTEventSystem::MQTTEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<types::URL> url) : MQTTEventSystem(std::move(logger)) {
  apply_url(std::move(url));
}

MQTTEventSystem::~MQTTEventSystem() { close_without_callback(); }

std::string MQTTEventSystem::name() const { return "mqtt"; }

std::string MQTTEventSystem::version() const { return "0.0.1"; }

void MQTTEventSystem::connect(plugins::Executor on, plugins::StatusHandler handler) {
  auto self = shared_from_this();

  bool expected = false;
  if (!_connected.compare_exchange_strong(expected, true)) {
    return _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  // A previous run that stopped from inside its own thread leaves the object joinable.
  if (_mqtt_thread.joinable()) _mqtt_thread.join();

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
    return _complete(std::move(on), std::move(handler), plugins::Status::failure(std::string("Failed to start MQTT thread: ") + e.what()));
  } catch (...) {
    _mqtt_work_guard.reset();
    _connected.store(false);
    _logger->error("Failed to start MQTT thread: unknown exception");
    return _complete(std::move(on), std::move(handler), plugins::Status::failure("Failed to start MQTT thread: unknown exception"));
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

  await_broker(std::move(on), std::move(handler));
}

// boost.mqtt5 is an always-online client: async_run starts it and it queues, retries
// and reconnects on its own, so nothing about starting it says the broker is there.
// Reporting success on that alone means a node with the wrong broker address starts
// up, says the event system connected, and has no bus at all - and once discovery runs
// on this bus that is a cluster that never forms and never says why.
//
// So connect() answers on a round trip. Subscribing to a filter nothing publishes to
// costs one SUBSCRIBE and one SUBACK, puts nothing on the bus, and cannot come back
// until the broker has answered. The wait is bounded, because an always-online client
// would otherwise keep trying for ever while main waits for it.
void MQTTEventSystem::await_broker(plugins::Executor on, plugins::StatusHandler handler) {
  auto self = shared_from_this();
  auto answered = std::make_shared<std::atomic_bool>(false);
  auto timer = std::make_shared<boost::asio::steady_timer>(_mqtt_strand);

  const auto probe = prefixed_topic(_prefix, "probe/" + _client_id);

  timer->expires_after(std::chrono::milliseconds(_connect_timeout_ms));
  timer->async_wait([this, self, answered, on, handler](const boost::system::error_code& ec) mutable {
    if (ec || answered->exchange(true)) return;

    _logger->error("No answer from the broker at " + _broker_host + ":" + std::to_string(_broker_port) + " within " + std::to_string(_connect_timeout_ms) +
                   "ms");

    close_without_callback();
    _complete(std::move(on), std::move(handler), plugins::Status::failure("no answer from the broker at " + _broker_host + ":" + std::to_string(_broker_port)));
  });

  boost::asio::dispatch(_mqtt_strand, [this, self, probe, answered, timer, on, handler]() mutable {
    std::vector<mqtt::subscribe_topic> topics{mqtt::subscribe_topic{
        probe, mqtt::subscribe_options{mqtt::qos_e::at_most_once, mqtt::no_local_e::no, mqtt::retain_as_published_e::retain, mqtt::retain_handling_e::send}}};

    _client.async_subscribe(
        std::move(topics), mqtt::subscribe_props{},
        [this, self, answered, timer, on, handler](mqtt::error_code ec, std::vector<mqtt::reason_code> reasons, mqtt::suback_props props) mutable {
          (void)reasons;
          (void)props;

          timer->cancel();
          if (answered->exchange(true)) return;

          if (ec) {
            _logger->error("Could not reach the broker: " + ec.message());
            close_without_callback();
            _complete(std::move(on), std::move(handler), plugins::Status::failure("could not reach the broker: " + ec.message()));
            return;
          }

          _logger->info("Connected to " + _broker_host + ":" + std::to_string(_broker_port) + " as " + _client_id);
          _complete(std::move(on), std::move(handler), plugins::Status::success());
        });
  });
}

void MQTTEventSystem::close() {
  const bool was_connected = _connected.exchange(false);

  if (was_connected) _logger->debug("Closing");

  stop_client();

  if (was_connected) _logger->info("Closed");
}

void MQTTEventSystem::close_without_callback() noexcept {
  try {
    stop_client();
  } catch (...) {
    // Destructors must not throw.
  }
}

// The flag and the thread are two different questions, and closing used to ask only
// the first. The client's own run loop clears _connected when the broker drops it, and
// after that every close was a no-op: the thread stayed joinable, and destroying a
// std::thread that is still joinable terminates the process. A node whose broker went
// away and was then shut down would die on the way out.
void MQTTEventSystem::stop_client() {
  _connected.store(false);

  if (!_mqtt_thread.joinable()) return;

  if (_mqtt_thread.get_id() == std::this_thread::get_id()) {
    // Called from the client's own thread, which cannot join itself. Ask it to finish
    // and let run() return; the join belongs to whoever owns this object.
    _client.async_disconnect([](mqtt::error_code) {});
    _mqtt_work_guard.reset();
    return;
  }

  boost::asio::post(_mqtt_strand, [this]() { _client.async_disconnect([](mqtt::error_code) {}); });
  _mqtt_work_guard.reset();
  _mqtt_thread.join();

  _broker_subscribed_events.clear();
  _events_by_identifier.clear();
  _receive_active = false;
}

bool MQTTEventSystem::is_connected() const { return _connected.load(); }

void MQTTEventSystem::publish(std::string event_name, std::string message) { publish_event(std::move(event_name), std::move(message), nullptr); }

void MQTTEventSystem::publish(plugins::Executor on, std::string event_name, std::string message, plugins::StatusHandler handler) {
  publish_event(std::move(event_name), std::move(message), bind_completion(std::move(on), std::move(handler)));
}

void MQTTEventSystem::publish_event(std::string event_name, std::string message, Completion completion) {
  auto self = shared_from_this();
  const auto topic = prefixed_topic(_prefix, event_name);

  if (!TopicFilter::is_valid_topic(topic)) {
    _logger->error("Publish rejected for invalid event topic: " + topic);
    finish(completion, plugins::Status::failure("Publish rejected for invalid event topic: " + topic));
    return;
  }

  if (!_connected.load()) {
    _logger->error("Publish rejected while closed: " + topic);
    finish(completion, plugins::Status::failure("Publish rejected while closed: " + topic));
    return;
  }

  _logger->debug("Publishing event: " + topic + " with message: " + message);

  boost::asio::dispatch(_mqtt_strand, [this, self, topic, message = std::move(message), completion = std::move(completion)]() mutable {
    if (!_connected.load()) {
      finish(completion, plugins::Status::failure("Publish rejected while closed: " + topic));
      return;
    }

    auto pub_callback = ([this, self, topic, completion = std::move(completion)](mqtt::error_code ec) mutable {
      if (ec) {
        _logger->error("MQTT publish failed: " + ec.message());
        finish(completion, plugins::Status::failure("MQTT publish failed for " + topic + ": " + ec.message()));
        return;
      }

      finish(completion, plugins::Status::success());
    });

    _client.async_publish<mqtt::qos_e::at_most_once>(topic, std::move(message), mqtt::retain_e::no, mqtt::publish_props{}, std::move(pub_callback));
  });
}

void MQTTEventSystem::subscribe(plugins::Executor on, std::string event_name, Subscription::EventCallbackFn event_callback,
                                plugins::Handler<std::shared_ptr<Subscription>> handler) {
  using SubscriptionResult = plugins::Result<std::shared_ptr<Subscription>>;

  auto self = shared_from_this();
  const auto topic_filter = prefixed_topic(_prefix, event_name);

  if (!TopicFilter::is_valid_filter(topic_filter)) {
    _logger->error("Subscribe rejected for invalid event filter: " + topic_filter);
    _complete(std::move(on), std::move(handler), SubscriptionResult::failure("Subscribe rejected for invalid event filter: " + topic_filter));
    return;
  }

  _logger->debug("Subscribing to event: " + topic_filter);
  auto sub = std::make_shared<Subscription>(std::move(event_name), std::move(event_callback));

  bool first_local_subscription = false;
  {
    std::scoped_lock lock(_subscriptions_mutex);
    auto& subscribers = _subscriptions[sub->event_name];
    first_local_subscription = subscribers.empty();
    subscribers.insert(sub);
  }

  if (!_connected.load() || !first_local_subscription) {
    _complete(std::move(on), std::move(handler), SubscriptionResult::success(sub));
    return;
  }

  // The handle only comes back once the broker has taken the subscription, and a
  // subscription the broker refused is dropped rather than left behind: a caller
  // holding a handle to nothing would never hear the events it asked for.
  auto answer = [this, self, sub, on = std::move(on), handler = std::move(handler)](plugins::Status status) mutable {
    if (!status.ok) {
      remove_subscription(sub);
      _complete(std::move(on), std::move(handler), SubscriptionResult::failure(std::move(status.error)));
      return;
    }

    _complete(std::move(on), std::move(handler), SubscriptionResult::success(sub));
  };

  boost::asio::dispatch(_mqtt_strand, [this, self, event_name = sub->event_name, answer = std::move(answer)]() mutable {
    subscribe_events_on_mqtt(std::vector<std::string>{std::move(event_name)}, std::move(answer));
  });
}

void MQTTEventSystem::unsubscribe(plugins::Executor on, std::shared_ptr<Subscription> subscription, plugins::StatusHandler handler) {
  if (!subscription) {
    _complete(std::move(on), std::move(handler), plugins::Status::failure("unsubscribe: no subscription"));
    return;
  }

  remove_subscription(subscription);

  // Deliberately do not UNSUBSCRIBE at the broker. Keeping the broker-side
  // subscription avoids races with a new local subscription to the same event.
  // Messages for events with no local subscribers are simply dropped.
  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

void MQTTEventSystem::unsubscribe_all(plugins::Executor on, plugins::StatusHandler handler) {
  _logger->debug("Unsubscribing all subscriptions");
  clear_local_subscriptions();
  _complete(std::move(on), std::move(handler), plugins::Status::success());
}

void MQTTEventSystem::apply_url(std::shared_ptr<types::URL> url) {
  _broker_host = "127.0.0.1";
  _broker_port = 1883;
  _client_id = default_client_id();
  _client_id_was_given = false;
  _username.clear();
  _password.clear();
  _keep_alive_seconds = 30;
  _connect_timeout_ms = 5000;

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
    _client_id_was_given = true;
  }

  it = params.find("keep_alive");
  if (it == params.end()) {
    it = params.find("keepalive");
  }
  if (it != params.end() && !it->second.empty()) {
    _keep_alive_seconds = parse_port(it->second, _keep_alive_seconds);
  }

  it = params.find("connect_timeout_ms");
  if (it != params.end() && !it->second.empty()) {
    try {
      const auto value = std::stol(it->second);
      if (value > 0) _connect_timeout_ms = static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
      _logger->warn("Ignoring an unreadable connect_timeout_ms: " + it->second);
    }
  }

  it = params.find("username");
  if (it != params.end()) {
    _username = it->second;
  }

  it = params.find("password");
  if (it != params.end()) {
    _password = it->second;
  }

  it = params.find("prefix");
  if (it != params.end()) {
    _prefix = it->second;
  }

  _prefix = normalise_prefix(std::move(_prefix));

  (void)path;
}

bool MQTTEventSystem::configure(const YAML::Node& own_root, const Config& system) {
  // A client identifier has to be unique on the broker, and the node id already is by
  // definition: two nodes sharing one is two nodes nobody can tell apart. Taking it
  // from there means an operator who configures nothing still gets a cluster whose
  // nodes do not disconnect each other, and one who wants to say it explicitly still
  // can.
  if (!_client_id_was_given && !system.sip_node_id.empty()) _client_id = "athenasip-" + system.sip_node_id;

  if (!own_root || !own_root.IsMap()) {
    return true;
  }

  // The URL selects the broker; anything else about the connection belongs here. The
  // query-string forms still parse, so an existing config keeps working, but the
  // section wins where both are given.
  if (own_root["client_id"]) {
    _client_id = own_root["client_id"].as<std::string>();
    _client_id_was_given = true;
  }

  if (own_root["keep_alive"]) _keep_alive_seconds = static_cast<std::uint16_t>(own_root["keep_alive"].as<int>());
  if (own_root["connect_timeout_ms"]) _connect_timeout_ms = static_cast<std::uint32_t>(own_root["connect_timeout_ms"].as<int>());
  if (own_root["username"]) _username = own_root["username"].as<std::string>();
  if (own_root["password"]) _password = own_root["password"].as<std::string>();
  if (own_root["prefix"]) _prefix = normalise_prefix(own_root["prefix"].as<std::string>());

  return true;
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

MQTTEventSystem::Completion MQTTEventSystem::bind_completion(plugins::Executor on, plugins::StatusHandler handler) {
  if (!handler) {
    return nullptr;
  }

  auto self = shared_from_this();
  return [self, on = std::move(on), handler = std::move(handler)](plugins::Status status) mutable { _complete(on, handler, std::move(status)); };
}

void MQTTEventSystem::finish(const Completion& completion, plugins::Status status) {
  if (completion) {
    completion(std::move(status));
  }
}

void MQTTEventSystem::clear_local_subscriptions() {
  std::scoped_lock lock(_subscriptions_mutex);
  _subscriptions.clear();
}

void MQTTEventSystem::remove_subscription(const std::shared_ptr<Subscription>& subscription) {
  std::scoped_lock lock(_subscriptions_mutex);

  auto it = _subscriptions.find(subscription->event_name);
  if (it == _subscriptions.end()) {
    return;
  }

  it->second.erase(subscription);
  if (it->second.empty()) {
    _subscriptions.erase(it);
  }
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

std::unordered_set<std::shared_ptr<Subscription>> MQTTEventSystem::collect_matching_subscriptions(std::string event_name) const {
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

void MQTTEventSystem::subscribe_events_on_mqtt(std::vector<std::string> event_names, Completion completion) {
  if (!_connected.load()) {
    finish(completion, plugins::Status::failure("subscribe rejected while closed"));
    return;
  }

  // Each filter goes in a SUBSCRIBE of its own, because a subscription identifier is a
  // property of the packet and applies to every filter in it. One packet per filter is
  // what lets a received message say which subscription it arrived for.
  std::vector<std::pair<std::string, std::string>> to_subscribe;  // logical, prefixed

  for (const auto& event_name : event_names) {
    auto topic_filter = prefixed_topic(_prefix, event_name);
    if (_broker_subscribed_events.insert(topic_filter).second) to_subscribe.emplace_back(event_name, std::move(topic_filter));
  }

  if (to_subscribe.empty()) {
    ensure_receive_loop();
    finish(completion, plugins::Status::success());
    return;
  }

  // One completion for the batch: it answers when the last SUBSCRIBE has, and reports
  // the first failure if there was one.
  auto outstanding = std::make_shared<std::size_t>(to_subscribe.size());
  auto failure = std::make_shared<std::string>();
  auto answer = std::make_shared<Completion>(std::move(completion));

  auto self = shared_from_this();

  for (auto& [event_name, topic_filter] : to_subscribe) {
    const auto identifier = _next_subscription_identifier++;
    _events_by_identifier[identifier] = event_name;

    _logger->info("subscribing to event: " + topic_filter);

    std::vector<mqtt::subscribe_topic> topics{mqtt::subscribe_topic{
        topic_filter,
        mqtt::subscribe_options{mqtt::qos_e::at_most_once, mqtt::no_local_e::no, mqtt::retain_as_published_e::retain, mqtt::retain_handling_e::send}}};

    mqtt::subscribe_props props;
    props[mqtt::prop::subscription_identifier] = identifier;

    _client.async_subscribe(std::move(topics), std::move(props),
                            [this, self, identifier, topic_filter, outstanding, failure, answer](mqtt::error_code ec, std::vector<mqtt::reason_code> reasons,
                                                                                                 mqtt::suback_props suback) mutable {
                              (void)suback;

                              const bool closed = !_connected.load();

                              if (closed || ec) {
                                _broker_subscribed_events.erase(topic_filter);
                                _events_by_identifier.erase(identifier);

                                if (failure->empty()) *failure = closed ? "subscribe rejected while closed" : ("subscribe failed: " + ec.message());
                                if (!closed) _logger->error("subscribe failed: " + ec.message());
                              } else {
                                // A broker that refuses a filter says so per topic. Holding on to a
                                // subscription it never made would leave the caller a handle to nothing.
                                for (const auto& reason : reasons) {
                                  if (reason) {
                                    _broker_subscribed_events.erase(topic_filter);
                                    _events_by_identifier.erase(identifier);

                                    if (failure->empty()) *failure = "subscribe refused: " + reason.message();
                                    _logger->error("subscribe refused for " + topic_filter + ": " + reason.message());
                                    break;
                                  }
                                }
                              }

                              if (--*outstanding > 0) return;

                              if (!failure->empty()) {
                                finish(*answer, plugins::Status::failure(*failure));
                                return;
                              }

                              ensure_receive_loop();
                              finish(*answer, plugins::Status::success());
                            });
  }
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
      _events_by_identifier.clear();
      subscribe_events_on_mqtt(current_subscription_events(), nullptr);
      return;
    }

    _logger->debug("received event: " + topic + " with message: " + payload);

    dispatch_event(std::move(topic), std::move(payload), props);
    ensure_receive_loop();
  });
}

void MQTTEventSystem::dispatch_event(std::string event_name, std::string message, const mqtt::publish_props& props) {
  if (!TopicFilter::is_valid_topic(event_name)) {
    _logger->error("Dropping invalid received event topic: " + event_name);
    return;
  }

  const auto mqtt_topic = event_name;
  if (!remove_prefix(_prefix, event_name)) {
    _logger->error("Dropping received event outside configured prefix: " + mqtt_topic);
    return;
  }

  if (!TopicFilter::is_valid_topic(event_name)) {
    _logger->error("Dropping received event with invalid logical topic: " + event_name);
    return;
  }

  // Which of this client's subscriptions the message arrived for, when the broker says
  // (MQTT 5 section 3.3.2.3.8). It may send one copy per matching subscription, each
  // naming its own, or one copy naming them all; both end up delivering to each
  // matching subscriber exactly once.
  //
  // Matching the topic against every local filter instead is what a client has to do
  // when the broker says nothing, and it is wrong whenever two of this client's filters
  // match: the broker sends a copy for each, and each copy is then fanned out to both
  // filters' subscribers.
  const auto& identifiers = props[mqtt::prop::subscription_identifier];

  if (!identifiers.empty()) {
    std::unordered_set<std::shared_ptr<Subscription>> to_publish;

    for (const auto identifier : identifiers) {
      const auto found = _events_by_identifier.find(identifier);
      if (found == _events_by_identifier.end()) continue;

      const auto subscribers = subscribers_of(found->second);
      to_publish.insert(subscribers.begin(), subscribers.end());
    }

    deliver_to(to_publish, event_name, message);
    return;
  }

  deliver_to(collect_matching_subscriptions(event_name), event_name, message);
}

std::unordered_set<std::shared_ptr<Subscription>> MQTTEventSystem::subscribers_of(const std::string& filter) const {
  std::scoped_lock lock(_subscriptions_mutex);

  const auto found = _subscriptions.find(filter);
  if (found == _subscriptions.end()) return {};

  return found->second;
}

void MQTTEventSystem::deliver_to(const std::unordered_set<std::shared_ptr<Subscription>>& subscribers, const std::string& event_name,
                                 const std::string& message) {
  for (const auto& sub : subscribers) {
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
