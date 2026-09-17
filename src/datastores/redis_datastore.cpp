//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "redis_datastore.h"

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/json.hpp>
#include <boost/redis/src.hpp>
#include <boost/system/system_error.hpp>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace athenasip::datastores {
namespace {

RedisError make_redis_response_error(const std::string& diagnostic) {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::protocol_error), diagnostic);
}

std::string json_string(const boost::json::object& obj, const char* key) {
  const auto* value = obj.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    throw std::runtime_error(std::string("Missing or non-string JSON field: ") + key);
  }

  return std::string(value->as_string().c_str());
}

std::uint64_t json_uint64(const boost::json::object& obj, const char* key) {
  const auto* value = obj.if_contains(key);
  if (value == nullptr) {
    throw std::runtime_error(std::string("Missing JSON field: ") + key);
  }

  if (value->is_uint64()) {
    return value->as_uint64();
  }

  if (value->is_int64()) {
    const auto signed_value = value->as_int64();
    if (signed_value < 0) {
      throw std::runtime_error(std::string("Negative value for unsigned JSON field: ") + key);
    }
    return static_cast<std::uint64_t>(signed_value);
  }

  if (value->is_string()) {
    return static_cast<std::uint64_t>(std::stoull(std::string(value->as_string().c_str())));
  }

  throw std::runtime_error(std::string("Non-integer JSON field: ") + key);
}

std::uint32_t json_uint32(const boost::json::object& obj, const char* key) {
  const auto value = json_uint64(obj, key);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string("JSON field out of uint32 range: ") + key);
  }
  return static_cast<std::uint32_t>(value);
}

std::uint16_t parse_port(std::string value) {
  if (value.empty()) {
    return 6379;
  }

  const auto parsed = std::stoul(value);
  if (parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("Redis URL port is out of range: " + value);
  }

  return static_cast<std::uint16_t>(parsed);
}

}  // namespace

RedisDatastore::RedisDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("redis_datastore", std::move(logger))), _url(std::move(url)) {
  _apply_url(_url);
}

RedisDatastore::~RedisDatastore() { close(); }

std::string RedisDatastore::get_driver_name() const { return "AthenaSIP Redis Driver v0.0.1"; }

bool RedisDatastore::connect() {
  if (_started.load()) {
    return _ping();
  }

  _logger->debug("URL: " + _host + ":" + std::to_string(_port));

  try {
    _work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(_io_context.get_executor());
    _connection = std::make_shared<boost::redis::connection>(_io_context.get_executor());

    auto cfg = _make_config();
    _connection->async_run(cfg, boost::asio::consign(boost::asio::detached, _connection));

    _io_thread = std::thread([this]() {
      try {
        _io_context.run();
      } catch (const std::exception& ex) {
        _logger->error(std::string("IO thread exception: ") + ex.what());
      } catch (...) {
        _logger->error("IO thread unknown exception.");
      }
    });

    _started.store(true);

    if (!_ping()) {
      _logger->error("Initial PING failed.");
      close();
      return false;
    }

    return true;
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    close();
    return false;
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    close();
    return false;
  }
}

void RedisDatastore::close() {
  if (!_started.exchange(false)) {
    return;
  }

  if (_connection) {
    _connection->cancel();
  }

  _work_guard.reset();
  _io_context.stop();

  if (_io_thread.joinable()) {
    _io_thread.join();
  }

  _connection.reset();
  _io_context.restart();
}

bool RedisDatastore::is_connected() const { return _started.load(); }

std::shared_ptr<types::Realm> RedisDatastore::realm_get_by_name(const std::string& realm_name) {
  try {
    const auto value = _get(_realm_key(realm_name));
    if (!value) {
      return nullptr;
    }

    const auto parsed = boost::json::parse(*value);
    const auto& obj = parsed.as_object();

    auto realm = std::make_shared<types::Realm>(json_string(obj, "name"));
    realm->id = json_uint64(obj, "id");
    realm->nonce_secret = json_string(obj, "nonce_secret");
    realm->nonce_expiry = json_uint32(obj, "nonce_expiry");
    realm->registration_timeout = json_uint32(obj, "registration_timeout");
    return realm;
  } catch (const std::exception& ex) {
    _logger->error("realm_get_by_name: " + std::string(ex.what()));
    return nullptr;
  }
}

std::string RedisDatastore::_serialise_realm(const std::shared_ptr<types::Realm>& realm) {
  boost::json::object obj;
  obj["id"] = realm->id;
  obj["name"] = realm->name;
  obj["nonce_secret"] = realm->nonce_secret;
  obj["nonce_expiry"] = realm->nonce_expiry;
  obj["registration_timeout"] = realm->registration_timeout;
  return boost::json::serialize(obj);
}

std::shared_ptr<types::Realm> RedisDatastore::_parse_realm(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto realm = std::make_shared<types::Realm>(json_string(obj, "name"));
  realm->id = json_uint64(obj, "id");
  realm->nonce_secret = json_string(obj, "nonce_secret");
  realm->nonce_expiry = json_uint32(obj, "nonce_expiry");
  realm->registration_timeout = json_uint32(obj, "registration_timeout");
  return realm;
}

bool RedisDatastore::realm_create(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  try {
    // create is not update: an existing realm is a conflict.
    if (_exists(_realm_key(realm->name))) return false;

    if (!_set(_realm_key(realm->name), _serialise_realm(realm))) return false;

    _sadd(_realm_index_key(), realm->name);
    return true;
  } catch (const std::exception& ex) {
    _logger->error("realm_create: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::realm_update(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  try {
    if (!_exists(_realm_key(realm->name))) return false;

    return _set(_realm_key(realm->name), _serialise_realm(realm));
  } catch (const std::exception& ex) {
    _logger->error("realm_update: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::realm_delete(const std::string& realm_name) {
  try {
    const bool removed = _del(_realm_key(realm_name)) > 0;
    _srem(_realm_index_key(), realm_name);
    return removed;
  } catch (const std::exception& ex) {
    _logger->error("realm_delete: " + std::string(ex.what()));
    return false;
  }
}

std::vector<std::shared_ptr<types::Realm>> RedisDatastore::realm_list() {
  std::vector<std::shared_ptr<types::Realm>> realms;

  try {
    for (const auto& name : _smembers(_realm_index_key())) {
      const auto value = _get(_realm_key(name));
      if (value) realms.push_back(_parse_realm(*value));
    }
  } catch (const std::exception& ex) {
    _logger->error("realm_list: " + std::string(ex.what()));
  }

  return realms;
}

std::string RedisDatastore::_serialise_subscriber(const std::shared_ptr<types::Subscriber>& subscriber) {
  boost::json::object obj;
  obj["id"] = subscriber->id;
  obj["ha1"] = subscriber->ha1;
  obj["uri"] = subscriber->identity->uri->to_string();
  return boost::json::serialize(obj);
}

bool RedisDatastore::subscriber_create(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto& uri = subscriber->identity->uri;

  try {
    if (_exists(_subscriber_key(uri->realm, uri->user))) return false;

    if (!_set(_subscriber_key(uri->realm, uri->user), _serialise_subscriber(subscriber))) return false;

    _sadd(_subscriber_index_key(uri->realm), uri->user);
    return true;
  } catch (const std::exception& ex) {
    _logger->error("subscriber_create: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::subscriber_update(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto& uri = subscriber->identity->uri;

  try {
    if (!_exists(_subscriber_key(uri->realm, uri->user))) return false;

    return _set(_subscriber_key(uri->realm, uri->user), _serialise_subscriber(subscriber));
  } catch (const std::exception& ex) {
    _logger->error("subscriber_update: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return false;

  const auto& uri = identity->uri;

  try {
    auto subscriber = subscriber_get(identity);

    const bool removed = _del(_subscriber_key(uri->realm, uri->user)) > 0;
    _srem(_subscriber_index_key(uri->realm), uri->user);

    // A deleted subscriber keeps no bindings.
    if (subscriber) {
      for (const auto& key : _smembers(_location_index_key(subscriber->id))) _del(key);
      _del(_location_index_key(subscriber->id));
    }

    return removed;
  } catch (const std::exception& ex) {
    _logger->error("subscriber_delete: " + std::string(ex.what()));
    return false;
  }
}

std::vector<std::shared_ptr<types::Subscriber>> RedisDatastore::subscriber_list(const std::string& realm_name) {
  std::vector<std::shared_ptr<types::Subscriber>> subscribers;

  try {
    for (const auto& user : _smembers(_subscriber_index_key(realm_name))) {
      auto identity = std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name);
      auto subscriber = subscriber_get(identity);
      if (subscriber) subscribers.push_back(subscriber);
    }
  } catch (const std::exception& ex) {
    _logger->error("subscriber_list: " + std::string(ex.what()));
  }

  return subscribers;
}

std::vector<types::Location> RedisDatastore::location_list(std::uint64_t subscriber_id) {
  std::vector<types::Location> locations;

  for (const auto& key : _smembers(_location_index_key(subscriber_id))) {
    try {
      const auto value = _get(key);

      // The binding expired and Redis dropped it; tidy the index as we go.
      if (!value) {
        _srem(_location_index_key(subscriber_id), key);
        continue;
      }

      const auto parsed = boost::json::parse(*value);
      const auto& obj = parsed.as_object();

      types::Location location;
      location.subscriber_id = json_uint64(obj, "subscriber_id");
      location.contact = std::make_shared<types::SIPUri>(json_string(obj, "contact"));
      location.registered_at = static_cast<std::time_t>(json_uint64(obj, "registered_at"));
      location.expires_at = static_cast<std::time_t>(json_uint64(obj, "expires_at"));
      location.nat = json_string(obj, "nat") == "Y";

      if (obj.if_contains("node_id")) location.node_id = json_string(obj, "node_id");
      if (obj.if_contains("flow_id")) location.flow_id = json_string(obj, "flow_id");
      if (obj.if_contains("path")) location.path = json_string(obj, "path");

      locations.push_back(std::move(location));
    } catch (const std::exception& ex) {
      // One unreadable binding must not hide the others: target determination needs
      // every contact it can get (RFC 3261 16.5).
      _logger->error("location_list: skipping " + key + ": " + std::string(ex.what()));
    }
  }

  return locations;
}

std::shared_ptr<types::Subscriber> RedisDatastore::subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  try {
    const auto value = _get(_subscriber_key(identity->uri->realm, identity->uri->user));
    if (!value) {
      return nullptr;
    }

    const auto parsed = boost::json::parse(*value);
    const auto& obj = parsed.as_object();

    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = json_uint64(obj, "id");
    subscriber->identity = std::move(identity);
    subscriber->ha1 = json_string(obj, "ha1");
    return subscriber;
  } catch (const std::exception& ex) {
    _logger->error("subscriber_get: " + std::string(ex.what()));
    return nullptr;
  }
}

bool RedisDatastore::subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  try {
    const std::uint16_t port = contact->port.value_or(0);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const bool is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm);

    // registration_timeout per realm arrives with the M4 location schema; until then
    // every binding gets the same lifetime, and Redis expires it for us.
    constexpr std::int64_t kDefaultRegistrationSeconds = 3600;

    boost::json::object location;
    location["subscriber_id"] = subscriber->id;
    location["contact"] = contact->to_string();
    location["user"] = contact->user;
    location["host"] = contact->realm;
    location["port"] = port;
    location["registered_at"] = now;
    location["expires_at"] = now + kDefaultRegistrationSeconds;
    location["nat"] = is_nat ? "Y" : "N";

    const auto key = _location_key(subscriber->id, contact->user, contact->realm, port);
    if (!_set_ex(key, boost::json::serialize(location), std::chrono::seconds(kDefaultRegistrationSeconds))) return false;

    // The index is what location_list reads, so listing never needs KEYS.
    _sadd(_location_index_key(subscriber->id), key);
    return true;
  } catch (const std::exception& ex) {
    _logger->error("subscriber_register: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  try {
    const std::uint16_t port = contact->port.value_or(0);
    const auto key = _location_key(subscriber->id, contact->user, contact->realm, port);

    const bool removed = _del(key) > 0;
    _srem(_location_index_key(subscriber->id), key);
    return removed;
  } catch (const std::exception& ex) {
    _logger->error("subscriber_unregister: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  const auto now = std::time(nullptr);
  if (expires_at <= now) {
    _logger->warn("nonce_create: refusing to create already-expired nonce");
    return false;
  }

  return _set_ex(_nonce_key(nonce), "1", std::chrono::seconds(expires_at - now));
}

bool RedisDatastore::nonce_check(std::string nonce) { return _exists(_nonce_key(nonce)); }

std::string RedisDatastore::_serialise_call(const std::shared_ptr<Call>& call) {
  boost::json::object obj;
  obj["id"] = call->id;
  obj["state"] = Call::state_to_string(call->state);
  obj["created_at"] = static_cast<std::uint64_t>(call->created_at);
  obj["answered_at"] = static_cast<std::uint64_t>(call->answered_at);
  obj["ended_at"] = static_cast<std::uint64_t>(call->ended_at);
  obj["focus"] = call->focus ? call->focus->to_string() : "";

  // Participants are persisted; their media streams are not. A relay set belongs to
  // the node that allocated it and cannot be handed to another one.
  boost::json::array participants;
  for (const auto& participant : call->participants) {
    boost::json::object entry;
    entry["identity"] = participant.identity ? participant.identity->to_string() : "";
    entry["node_id"] = participant.node_id;
    entry["local_tag"] = participant.local_tag;
    entry["remote_tag"] = participant.remote_tag;
    entry["originator"] = participant.originator;
    participants.push_back(std::move(entry));
  }

  obj["participants"] = std::move(participants);
  return boost::json::serialize(obj);
}

std::shared_ptr<Call> RedisDatastore::_parse_call(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto call = std::make_shared<Call>();
  call->id = json_string(obj, "id");
  call->created_at = static_cast<std::time_t>(json_uint64(obj, "created_at"));
  call->answered_at = static_cast<std::time_t>(json_uint64(obj, "answered_at"));
  call->ended_at = static_cast<std::time_t>(json_uint64(obj, "ended_at"));

  const auto state = json_string(obj, "state");
  for (const auto candidate :
       {Call::State::Initial, Call::State::Trying, Call::State::Ringing, Call::State::Connected, Call::State::Closing, Call::State::Closed}) {
    if (Call::state_to_string(candidate) == state) {
      call->state = candidate;
      break;
    }
  }

  const auto focus = json_string(obj, "focus");
  if (!focus.empty()) call->focus = std::make_shared<types::SIPUri>(focus);

  if (const auto* participants = obj.if_contains("participants"); participants != nullptr && participants->is_array()) {
    for (const auto& entry : participants->as_array()) {
      const auto& participant_obj = entry.as_object();

      auto& participant = call->add_participant(std::make_shared<types::SIPIdentity>(json_string(participant_obj, "identity")), nullptr,
                                                participant_obj.at("originator").as_bool());
      participant.node_id = json_string(participant_obj, "node_id");
      participant.local_tag = json_string(participant_obj, "local_tag");
      participant.remote_tag = json_string(participant_obj, "remote_tag");
    }
  }

  return call;
}

bool RedisDatastore::call_create(std::shared_ptr<Call> call) {
  if (!call || call->id.empty()) return false;

  try {
    if (!_set(_call_key(call->id), _serialise_call(call))) return false;

    _sadd(_call_index_key(), call->id);
    return true;
  } catch (const std::exception& ex) {
    _logger->error("call_create: " + std::string(ex.what()));
    return false;
  }
}

bool RedisDatastore::call_update(std::shared_ptr<Call> call) {
  if (!call || call->id.empty()) return false;

  try {
    if (!_exists(_call_key(call->id))) return false;

    return _set(_call_key(call->id), _serialise_call(call));
  } catch (const std::exception& ex) {
    _logger->error("call_update: " + std::string(ex.what()));
    return false;
  }
}

std::shared_ptr<Call> RedisDatastore::call_get(const std::string& id) {
  try {
    const auto value = _get(_call_key(id));
    if (!value) return nullptr;

    return _parse_call(*value);
  } catch (const std::exception& ex) {
    _logger->error("call_get: " + std::string(ex.what()));
    return nullptr;
  }
}

std::vector<std::shared_ptr<Call>> RedisDatastore::call_list() {
  std::vector<std::shared_ptr<Call>> calls;

  try {
    for (const auto& id : _smembers(_call_index_key())) {
      const auto value = _get(_call_key(id));

      if (!value) {
        _srem(_call_index_key(), id);
        continue;
      }

      calls.push_back(_parse_call(*value));
    }
  } catch (const std::exception& ex) {
    _logger->error("call_list: " + std::string(ex.what()));
  }

  return calls;
}

void RedisDatastore::_apply_url(std::shared_ptr<types::URL> url) {
  _host = "127.0.0.1";
  _port = 6379;
  _username = "default";
  _password.clear();
  _database_index = 0;
  _use_ssl = false;

  if (!url) {
    return;
  }

  std::string value = url->to_string();

  const auto scheme_pos = value.find("://");
  std::string scheme;
  if (scheme_pos != std::string::npos) {
    scheme = value.substr(0, scheme_pos);
    value.erase(0, scheme_pos + 3);
  } else {
    scheme = url->scheme;
  }

  _use_ssl = scheme == "rediss" || scheme == "redis+ssl";

  const auto path_pos = value.find('/');
  std::string authority = path_pos == std::string::npos ? value : value.substr(0, path_pos);
  std::string path = path_pos == std::string::npos ? std::string{} : value.substr(path_pos + 1);

  const auto query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    path.erase(query_pos);
  }

  if (!path.empty()) {
    _database_index = static_cast<std::int32_t>(std::stol(path));
  }

  const auto auth_pos = authority.rfind('@');
  if (auth_pos != std::string::npos) {
    const auto auth = authority.substr(0, auth_pos);
    authority.erase(0, auth_pos + 1);

    const auto colon_pos = auth.find(':');
    if (colon_pos == std::string::npos) {
      _password = auth;
    } else {
      _username = auth.substr(0, colon_pos);
      _password = auth.substr(colon_pos + 1);
      if (_username.empty()) {
        _username = "default";
      }
    }
  }

  const auto port_pos = authority.rfind(':');
  if (port_pos == std::string::npos) {
    if (!authority.empty()) {
      _host = authority;
    }
    return;
  }

  _host = authority.substr(0, port_pos);
  _port = parse_port(authority.substr(port_pos + 1));

  if (_host.empty()) {
    _host = "127.0.0.1";
  }
}

boost::redis::config RedisDatastore::_make_config() const {
  boost::redis::config cfg;
  cfg.addr.host = _host;
  cfg.addr.port = std::to_string(_port);
  cfg.username = _username;
  cfg.password = _password;
  cfg.database_index = _database_index;
  cfg.use_ssl = _use_ssl;
  return cfg;
}

bool RedisDatastore::_ping() {
  return _wait_bool([this](BoolCallback callback) { _async_ping(std::move(callback)); });
}

bool RedisDatastore::_set(std::string key, std::string value) {
  return _wait_bool([this, key = std::move(key), value = std::move(value)](BoolCallback callback) mutable {
    _async_set(std::move(key), std::move(value), std::move(callback));
  });
}

bool RedisDatastore::_set_ex(std::string key, std::string value, std::chrono::seconds expiry) {
  return _wait_bool([this, key = std::move(key), value = std::move(value), expiry](BoolCallback callback) mutable {
    _async_set_ex(std::move(key), std::move(value), expiry, std::move(callback));
  });
}

std::optional<std::string> RedisDatastore::_get(std::string key) {
  return _wait_string([this, key = std::move(key)](StringCallback callback) mutable { _async_get(std::move(key), std::move(callback)); });
}

std::int64_t RedisDatastore::_del(std::string key) {
  return _wait_integer([this, key = std::move(key)](IntegerCallback callback) mutable { _async_del(std::move(key), std::move(callback)); });
}

bool RedisDatastore::_exists(std::string key) {
  return _wait_bool([this, key = std::move(key)](BoolCallback callback) mutable { _async_exists(std::move(key), std::move(callback)); });
}

void RedisDatastore::_async_ping(BoolCallback callback) {
  boost::redis::request req;
  req.push("PING");
  _async_ok("PING", std::move(req), std::move(callback));
}

void RedisDatastore::_async_set(std::string key, std::string value, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value);
  _async_ok("SET", std::move(req), std::move(callback));
}

void RedisDatastore::_async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value, "EX", expiry.count());
  _async_ok("SET EX", std::move(req), std::move(callback));
}

void RedisDatastore::_async_get(std::string key, StringCallback callback) {
  boost::redis::request req;
  req.push("GET", key);
  _async_string("GET", std::move(req), std::move(callback));
}

void RedisDatastore::_async_del(std::string key, IntegerCallback callback) {
  boost::redis::request req;
  req.push("DEL", key);
  _async_integer("DEL", std::move(req), std::move(callback));
}

void RedisDatastore::_async_exists(std::string key, BoolCallback callback) {
  boost::redis::request req;
  req.push("EXISTS", key);

  _async_integer("EXISTS", std::move(req),
                 [callback = std::move(callback)](RedisError error, std::int64_t value) mutable { callback(std::move(error), value > 0); });
}

void RedisDatastore::_async_ok(std::string operation, boost::redis::request request, BoolCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), false);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::string>>();

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), false);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), false);
            return;
          }

          const auto& value = result.value();
          const bool ok = value == "OK" || value == "PONG";

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, ok);
        });
  });
}

void RedisDatastore::_async_string(std::string operation, boost::redis::request request, StringCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), std::nullopt);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), std::nullopt);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), std::nullopt);
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, result.value());
        });
  });
}

void RedisDatastore::_async_integer(std::string operation, boost::redis::request request, IntegerCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), 0);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<long long>>();

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), 0);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), 0);
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, static_cast<std::int64_t>(result.value()));
        });
  });
}

bool RedisDatastore::_sadd(std::string key, std::string member) {
  boost::redis::request request;
  request.push("SADD", key, member);

  return _wait_integer(
             [this, request = std::move(request)](IntegerCallback callback) mutable { _async_integer("SADD", std::move(request), std::move(callback)); }) >= 0;
}

bool RedisDatastore::_srem(std::string key, std::string member) {
  boost::redis::request request;
  request.push("SREM", key, member);

  return _wait_integer(
             [this, request = std::move(request)](IntegerCallback callback) mutable { _async_integer("SREM", std::move(request), std::move(callback)); }) > 0;
}

std::vector<std::string> RedisDatastore::_smembers(std::string key) {
  boost::redis::request request;
  request.push("SMEMBERS", key);

  return _wait_strings(
      [this, request = std::move(request)](StringsCallback callback) mutable { _async_strings("SMEMBERS", std::move(request), std::move(callback)); });
}

void RedisDatastore::_async_strings(std::string operation, boost::redis::request request, StringsCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), {});
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), {});
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), {});
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, result.value());
        });
  });
}

std::vector<std::string> RedisDatastore::_wait_strings(std::function<void(StringsCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, std::vector<std::string>>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, std::vector<std::string> value) mutable { promise->set_value({std::move(error), std::move(value)}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return {};
  }

  auto [error, value] = future.get();
  return error ? std::vector<std::string>{} : value;
}

bool RedisDatastore::_wait_bool(std::function<void(BoolCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, bool>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, bool value) mutable { promise->set_value({std::move(error), value}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return false;
  }

  auto [error, value] = future.get();
  return !error && value;
}

std::optional<std::string> RedisDatastore::_wait_string(std::function<void(StringCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, std::optional<std::string>>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, std::optional<std::string> value) mutable { promise->set_value({std::move(error), std::move(value)}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return std::nullopt;
  }

  auto [error, value] = future.get();
  return error ? std::nullopt : value;
}

std::int64_t RedisDatastore::_wait_integer(std::function<void(IntegerCallback)> starter) {
  auto promise = std::make_shared<std::promise<std::pair<RedisError, std::int64_t>>>();
  auto future = promise->get_future();

  starter([promise](RedisError error, std::int64_t value) mutable { promise->set_value({std::move(error), value}); });

  if (future.wait_for(_sync_timeout) != std::future_status::ready) {
    auto error = _timeout_error();
    _logger->error("Sync timeout: " + error.message());
    return 0;
  }

  auto [error, value] = future.get();
  return error ? 0 : value;
}

std::string RedisDatastore::_realm_key(const std::string& realm_name) { return "athena:realm:" + realm_name; }

std::string RedisDatastore::_call_key(const std::string& call_id) { return "athena:call:" + call_id; }

std::string RedisDatastore::_realm_index_key() { return "athena:index:realms"; }

std::string RedisDatastore::_subscriber_index_key(const std::string& realm_name) { return "athena:index:subscribers:" + realm_name; }

std::string RedisDatastore::_location_index_key(std::uint64_t subscriber_id) { return "athena:index:locations:" + std::to_string(subscriber_id); }

std::string RedisDatastore::_call_index_key() { return "athena:index:calls"; }

std::string RedisDatastore::_subscriber_key(const std::string& realm_name, const std::string& user) { return "athena:subscriber:" + realm_name + ":" + user; }

std::string RedisDatastore::_location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port) {
  return "athena:location:" + std::to_string(subscriber_id) + ":" + user + ":" + host + ":" + std::to_string(port);
}

std::string RedisDatastore::_nonce_key(const std::string& nonce) { return "athena:nonce:" + nonce; }

std::string RedisDatastore::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

RedisError RedisDatastore::_not_connected_error() const {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::not_connected), "Redis connection is not started");
}

RedisError RedisDatastore::_timeout_error() const {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::timed_out), "Redis synchronous command timed out");
}

}  // namespace athenasip::datastores
