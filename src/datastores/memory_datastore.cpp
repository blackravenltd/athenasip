//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "memory_datastore.h"

#include <utility>

#include "../util.h"

namespace athenasip::datastores {

namespace {

// Used when a contact's realm has no Realm record to take a registration timeout from.
constexpr std::time_t kDefaultRegistrationSeconds = 3600;

}  // namespace

MemoryDatastore::MemoryDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("memory_datastore", std::move(logger))), _url(std::move(url)) {}

MemoryDatastore::~MemoryDatastore() { close(); }

std::string MemoryDatastore::name() const { return "memory"; }

std::string MemoryDatastore::version() const { return "0.0.1"; }

bool MemoryDatastore::connect() {
  std::lock_guard<std::mutex> lock(_mutex);
  _connected = true;
  _logger->info("Connected");
  return true;
}

void MemoryDatastore::close() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_connected) return;

  _connected = false;
  _logger->info("Closed");
}

bool MemoryDatastore::is_connected() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _connected;
}

std::shared_ptr<types::Realm> MemoryDatastore::realm_get_by_name(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _realms.find(realm_name);
  return it == _realms.end() ? nullptr : it->second;
}

bool MemoryDatastore::realm_create(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  // create is not update: an existing realm is a conflict, not an overwrite.
  if (_realms.find(realm->name) != _realms.end()) return false;

  _realms[realm->name] = std::move(realm);
  return true;
}

bool MemoryDatastore::realm_update(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  if (_realms.find(realm->name) == _realms.end()) return false;

  _realms[realm->name] = std::move(realm);
  return true;
}

bool MemoryDatastore::realm_delete(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _realms.erase(realm_name) > 0;
}

std::vector<std::shared_ptr<types::Realm>> MemoryDatastore::realm_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Realm>> realms;
  realms.reserve(_realms.size());
  for (const auto& [name, realm] : _realms) realms.push_back(realm);

  return realms;
}

std::shared_ptr<types::Subscriber> MemoryDatastore::subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return nullptr;

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _subscribers.find(_subscriber_key(identity->uri->realm, identity->uri->user));
  if (it == _subscribers.end()) return nullptr;

  // Hand back the identity the caller asked with, as the Redis driver does, so the
  // returned subscriber carries the tags of this request.
  auto subscriber = std::make_shared<types::Subscriber>();
  subscriber->id = it->second->id;
  subscriber->ha1 = it->second->ha1;
  subscriber->identity = std::move(identity);
  return subscriber;
}

bool MemoryDatastore::subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact, std::uint32_t expires_seconds,
                                          const std::string& path) {
  if (!subscriber || !contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  // The registrar negotiated this lifetime with the client and told the client about it
  // in the 200 OK, so the binding has to expire when it said it would. A caller that
  // asks for nothing gets the built-in default (RFC 3261 10.2.1).
  const std::time_t ttl = expires_seconds > 0 ? static_cast<std::time_t>(expires_seconds) : kDefaultRegistrationSeconds;

  const std::uint16_t port = contact->port.value_or(0);

  const auto now = std::time(nullptr);

  types::Location location;
  location.contact = contact;
  location.subscriber_id = subscriber->id;
  location.registered_at = now;
  location.expires_at = now + ttl;
  location.path = path;
  location.nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm);

  _locations[_location_key(subscriber->id, contact->user, contact->realm, port)] = std::move(location);
  return true;
}

bool MemoryDatastore::subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  if (!subscriber || !contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  const std::uint16_t port = contact->port.value_or(0);
  return _locations.erase(_location_key(subscriber->id, contact->user, contact->realm, port)) > 0;
}

std::vector<types::Location> MemoryDatastore::location_list(std::uint64_t subscriber_id) {
  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  std::vector<types::Location> locations;
  for (const auto& [key, location] : _locations) {
    if (location.subscriber_id == subscriber_id) locations.push_back(location);
  }

  return locations;
}

bool MemoryDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  const auto now = std::time(nullptr);

  if (expires_at <= now) {
    _logger->warn("nonce_create: refusing to create already-expired nonce");
    return false;
  }

  std::lock_guard<std::mutex> lock(_mutex);
  _nonces[nonce] = expires_at;
  return true;
}

bool MemoryDatastore::nonce_check(std::string nonce) {
  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  return _nonces.find(nonce) != _nonces.end();
}

bool MemoryDatastore::call_create(std::shared_ptr<Call> call) {
  if (!call || call->id.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);
  _calls[call->id] = std::move(call);
  return true;
}

bool MemoryDatastore::call_update(std::shared_ptr<Call> call) {
  if (!call) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  if (_calls.find(call->id) == _calls.end()) return false;

  _calls[call->id] = std::move(call);
  return true;
}

std::shared_ptr<Call> MemoryDatastore::call_get(const std::string& id) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _calls.find(id);
  return it == _calls.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Call>> MemoryDatastore::call_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<Call>> calls;
  calls.reserve(_calls.size());
  for (const auto& [id, call] : _calls) calls.push_back(call);

  return calls;
}

bool MemoryDatastore::subscriber_create(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto key = _subscriber_key(subscriber->identity->uri->realm, subscriber->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_subscribers.find(key) != _subscribers.end()) return false;

  _subscribers[key] = std::move(subscriber);
  return true;
}

bool MemoryDatastore::subscriber_update(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto key = _subscriber_key(subscriber->identity->uri->realm, subscriber->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_subscribers.find(key) == _subscribers.end()) return false;

  _subscribers[key] = std::move(subscriber);
  return true;
}

bool MemoryDatastore::subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return false;

  const auto key = _subscriber_key(identity->uri->realm, identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  auto subscriber = _subscribers.find(key);
  if (subscriber == _subscribers.end()) return false;

  const auto subscriber_id = subscriber->second->id;
  _subscribers.erase(subscriber);

  // A deleted subscriber keeps no bindings.
  for (auto it = _locations.begin(); it != _locations.end();) {
    it = (it->second.subscriber_id == subscriber_id) ? _locations.erase(it) : std::next(it);
  }

  return true;
}

std::vector<std::shared_ptr<types::Subscriber>> MemoryDatastore::subscriber_list(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Subscriber>> subscribers;
  for (const auto& [key, subscriber] : _subscribers) {
    if (realm_name.empty() || (subscriber->identity && subscriber->identity->uri && subscriber->identity->uri->realm == realm_name)) {
      subscribers.push_back(subscriber);
    }
  }

  return subscribers;
}

std::string MemoryDatastore::_subscriber_key(const std::string& realm_name, const std::string& user) { return realm_name + ":" + user; }

std::string MemoryDatastore::_location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port) {
  return std::to_string(subscriber_id) + ":" + user + ":" + host + ":" + std::to_string(port);
}

void MemoryDatastore::_prune_expired() {
  const auto now = std::time(nullptr);

  for (auto it = _locations.begin(); it != _locations.end();) {
    it = (it->second.expires_at <= now) ? _locations.erase(it) : std::next(it);
  }

  for (auto it = _nonces.begin(); it != _nonces.end();) {
    it = (it->second <= now) ? _nonces.erase(it) : std::next(it);
  }
}

}  // namespace athenasip::datastores
