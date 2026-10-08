//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "memory_datastore.h"

#include <boost/asio/post.hpp>
#include <set>
#include <utility>

#include "../config.h"
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

void MemoryDatastore::connect(plugins::Executor on, plugins::StatusHandler handler) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _connected = true;
  }

  _logger->info("Connected");
  _complete(std::move(on), std::move(handler), plugins::Status::success());
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

std::shared_ptr<types::Realm> MemoryDatastore::_realm_get_by_name(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _realms.find(realm_name);
  return it == _realms.end() ? nullptr : it->second;
}

bool MemoryDatastore::_realm_create(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  // An existing realm is a conflict, not an overwrite.
  if (_realms.find(realm->name) != _realms.end()) return false;

  _realms[realm->name] = std::move(realm);
  return true;
}

bool MemoryDatastore::_realm_update(std::shared_ptr<types::Realm> realm) {
  if (!realm || realm->name.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  if (_realms.find(realm->name) == _realms.end()) return false;

  _realms[realm->name] = std::move(realm);
  return true;
}

bool MemoryDatastore::_realm_delete(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  if (_realms.erase(realm_name) == 0) return false;

  // Its subscribers and their bindings go with it.
  std::set<std::uint64_t> gone;
  for (auto it = _subscribers.begin(); it != _subscribers.end();) {
    const auto& subscriber = it->second;
    if (subscriber->identity && subscriber->identity->uri && subscriber->identity->uri->host == realm_name) {
      gone.insert(subscriber->id);
      it = _subscribers.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = _locations.begin(); it != _locations.end();) {
    it = gone.count(it->second.subscriber_id) ? _locations.erase(it) : std::next(it);
  }

  return true;
}

std::vector<std::shared_ptr<types::Realm>> MemoryDatastore::_realm_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Realm>> realms;
  realms.reserve(_realms.size());
  for (const auto& [name, realm] : _realms) realms.push_back(realm);

  return realms;
}

// Returns a copy, as a networked driver must: a caller that changes a user without
// writing it back must not change the stored record.
std::shared_ptr<types::User> MemoryDatastore::_user_get(const std::string& username) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _users.find(types::User::normalise(username));
  return it == _users.end() ? nullptr : std::make_shared<types::User>(*it->second);
}

bool MemoryDatastore::_user_create(std::shared_ptr<types::User> user) {
  if (!user || user->username.empty()) return false;

  const auto key = user->key();

  std::lock_guard<std::mutex> lock(_mutex);

  // An existing user is a conflict. The key is case-folded, so "Tom" and "tom" collide.
  if (_users.find(key) != _users.end()) return false;

  _users[key] = std::make_shared<types::User>(*user);
  return true;
}

bool MemoryDatastore::_user_update(std::shared_ptr<types::User> user) {
  if (!user || user->username.empty()) return false;

  const auto key = user->key();

  std::lock_guard<std::mutex> lock(_mutex);

  if (_users.find(key) == _users.end()) return false;

  _users[key] = std::make_shared<types::User>(*user);
  return true;
}

bool MemoryDatastore::_user_delete(const std::string& username) {
  const auto key = types::User::normalise(username);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_users.erase(key) == 0) return false;

  // A session whose user is gone could not be revoked by username.
  _session_erase_for_user(key);
  return true;
}

std::vector<std::shared_ptr<types::User>> MemoryDatastore::_user_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::User>> users;
  users.reserve(_users.size());
  for (const auto& [key, user] : _users) users.push_back(std::make_shared<types::User>(*user));

  return users;
}

// Creating over a held hash replaces the record, which is how last_seen_at moves: the
// contract has no session_update.
bool MemoryDatastore::_session_create(types::Session session) {
  if (session.token_hash.empty() || session.username.empty()) return false;

  // An already-expired session is a caller bug and is refused, as Redis must: SETEX takes
  // no non-positive expiry.
  if (session.expires_at <= std::time(nullptr)) {
    _logger->warn("session_create: refusing to create already-expired session");
    return false;
  }

  // Filed under the user's case-folded key, so revoking by username finds it.
  session.username = types::User::normalise(session.username);

  const auto key = session.token_hash;

  std::lock_guard<std::mutex> lock(_mutex);
  _sessions[key] = std::move(session);
  return true;
}

std::shared_ptr<types::Session> MemoryDatastore::_session_get(const std::string& token_hash) {
  if (token_hash.empty()) return nullptr;

  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  auto it = _sessions.find(token_hash);
  return it == _sessions.end() ? nullptr : std::make_shared<types::Session>(it->second);
}

// Succeeds whether or not the hash was held, as the contract requires.
bool MemoryDatastore::_session_delete(const std::string& token_hash) {
  std::lock_guard<std::mutex> lock(_mutex);
  _sessions.erase(token_hash);
  return true;
}

// Having nothing to revoke is success.
bool MemoryDatastore::_session_delete_for_user(const std::string& username) {
  std::lock_guard<std::mutex> lock(_mutex);

  _session_erase_for_user(types::User::normalise(username));
  return true;
}

std::size_t MemoryDatastore::_session_erase_for_user(const std::string& key) {
  std::size_t erased = 0;

  for (auto it = _sessions.begin(); it != _sessions.end();) {
    if (it->second.username == key) {
      it = _sessions.erase(it);
      ++erased;
    } else {
      it = std::next(it);
    }
  }

  return erased;
}

std::shared_ptr<types::Subscriber> MemoryDatastore::_subscriber_get(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return nullptr;

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _subscribers.find(_subscriber_key(identity->uri->host, identity->uri->user));
  if (it == _subscribers.end()) return nullptr;

  // A full copy of the stored subscriber, carrying the identity the caller asked with (and
  // so this request's tags), as the Redis driver returns.
  auto subscriber = std::make_shared<types::Subscriber>(*it->second);
  subscriber->identity = std::move(identity);
  return subscriber;
}

bool MemoryDatastore::_subscriber_register(const std::shared_ptr<types::Subscriber>& subscriber, types::Location binding, std::uint32_t expires_seconds) {
  if (!subscriber || !binding.contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  // The registrar told the client this lifetime in the 200 OK, so the binding expires on
  // it. Zero takes the default (RFC 3261 10.2.1).
  const std::time_t ttl = expires_seconds > 0 ? static_cast<std::time_t>(expires_seconds) : kDefaultRegistrationSeconds;

  const auto contact = binding.contact;
  const std::uint16_t port = contact->port.value_or(0);

  const auto now = std::time(nullptr);

  // The store settles the lifetime and the identity; the rest is kept as given.
  binding.subscriber_id = subscriber->id;
  binding.registered_at = now;
  binding.expires_at = now + ttl;
  binding.nat = Util::is_ipv4(contact->host) && Util::is_ipv4_private(contact->host);

  _locations[_location_key(subscriber->id, contact->user, contact->host, port)] = std::move(binding);
  return true;
}

bool MemoryDatastore::_subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) {
  if (!subscriber || !contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  const std::uint16_t port = contact->port.value_or(0);
  return _locations.erase(_location_key(subscriber->id, contact->user, contact->host, port)) > 0;
}

std::vector<types::Location> MemoryDatastore::_location_list(std::uint64_t subscriber_id) {
  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  std::vector<types::Location> locations;
  for (const auto& [key, location] : _locations) {
    if (location.subscriber_id == subscriber_id) locations.push_back(location);
  }

  return locations;
}

bool MemoryDatastore::_nonce_create(const std::string& nonce, const std::time_t& expires_at) {
  const auto now = std::time(nullptr);

  if (expires_at <= now) {
    _logger->warn("nonce_create: refusing to create already-expired nonce");
    return false;
  }

  std::lock_guard<std::mutex> lock(_mutex);
  _nonces[nonce] = expires_at;
  return true;
}

bool MemoryDatastore::_nonce_check(std::string nonce) {
  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  return _nonces.find(nonce) != _nonces.end();
}

bool MemoryDatastore::configure(const YAML::Node& own_root, const Config& system) {
  (void)own_root;
  _call_retention = system.calls_history_retention;
  return true;
}

// Drops call records older than the retention. Called with the lock held, when a call is
// added.
void MemoryDatastore::_call_prune(std::time_t now) {
  if (_call_retention == 0) return;

  for (auto it = _calls.begin(); it != _calls.end();) {
    const auto& call = it->second;
    const bool expired = call->state == Call::State::Closed && call->ended_at != 0 && now - call->ended_at > static_cast<std::time_t>(_call_retention);
    it = expired ? _calls.erase(it) : std::next(it);
  }
}

bool MemoryDatastore::_call_create(std::shared_ptr<Call> call) {
  if (!call || call->id.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);
  _call_prune(std::time(nullptr));
  _calls[call->id] = std::move(call);
  return true;
}

bool MemoryDatastore::_call_update(std::shared_ptr<Call> call) {
  if (!call) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  if (_calls.find(call->id) == _calls.end()) return false;

  _calls[call->id] = std::move(call);
  return true;
}

std::shared_ptr<Call> MemoryDatastore::_call_get(const std::string& id) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _calls.find(id);
  return it == _calls.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Call>> MemoryDatastore::_call_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<Call>> calls;
  calls.reserve(_calls.size());
  for (const auto& [id, call] : _calls) calls.push_back(call);

  return calls;
}

bool MemoryDatastore::_subscriber_create(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto key = _subscriber_key(subscriber->identity->uri->host, subscriber->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_subscribers.find(key) != _subscribers.end()) return false;

  _subscribers[key] = std::move(subscriber);
  return true;
}

bool MemoryDatastore::_subscriber_update(std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return false;

  const auto key = _subscriber_key(subscriber->identity->uri->host, subscriber->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_subscribers.find(key) == _subscribers.end()) return false;

  _subscribers[key] = std::move(subscriber);
  return true;
}

bool MemoryDatastore::_subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return false;

  const auto key = _subscriber_key(identity->uri->host, identity->uri->user);

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

std::vector<std::shared_ptr<types::Subscriber>> MemoryDatastore::_subscriber_list(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Subscriber>> subscribers;
  for (const auto& [key, subscriber] : _subscribers) {
    if (realm_name.empty() || (subscriber->identity && subscriber->identity->uri && subscriber->identity->uri->host == realm_name)) {
      subscribers.push_back(subscriber);
    }
  }

  return subscribers;
}

// The contract: answers are posted to the caller's executor, never delivered inline.
void MemoryDatastore::realm_get_by_name(plugins::Executor on, std::string realm_name, plugins::Handler<std::shared_ptr<types::Realm>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::Realm>>::success(_realm_get_by_name(realm_name)));
}

void MemoryDatastore::realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_realm_create(std::move(realm)), "realm_create"));
}

void MemoryDatastore::realm_update(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_realm_update(std::move(realm)), "realm_update"));
}

void MemoryDatastore::realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_realm_delete(realm_name), "realm_delete"));
}

void MemoryDatastore::realm_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Realm>>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<types::Realm>>>::success(_realm_list()));
}

// Copies in and out, as a networked driver gives: a caller that changes a trunk without writing it back changes
// nothing stored.
void MemoryDatastore::trunk_get(plugins::Executor on, std::string name, plugins::Handler<std::shared_ptr<types::Trunk>> handler) {
  std::shared_ptr<types::Trunk> found;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (const auto it = _trunks.find(types::Trunk::normalise(name)); it != _trunks.end()) found = std::make_shared<types::Trunk>(it->second);
  }
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::Trunk>>::success(std::move(found)));
}

void MemoryDatastore::trunk_create(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
  bool created = false;
  if (trunk && !trunk->name.empty()) {
    std::lock_guard<std::mutex> lock(_mutex);
    created = _trunks.emplace(trunk->key(), *trunk).second;
  }
  _complete(std::move(on), std::move(handler), _status(created, "trunk_create"));
}

void MemoryDatastore::trunk_update(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
  bool updated = false;
  if (trunk) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (auto it = _trunks.find(trunk->key()); it != _trunks.end()) {
      it->second = *trunk;
      updated = true;
    }
  }
  _complete(std::move(on), std::move(handler), _status(updated, "trunk_update"));
}

void MemoryDatastore::trunk_delete(plugins::Executor on, std::string name, plugins::StatusHandler handler) {
  bool deleted = false;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    deleted = _trunks.erase(types::Trunk::normalise(name)) != 0;
  }
  _complete(std::move(on), std::move(handler), _status(deleted, "trunk_delete"));
}

void MemoryDatastore::trunk_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Trunk>>> handler) {
  std::vector<std::shared_ptr<types::Trunk>> trunks;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& [key, trunk] : _trunks) trunks.push_back(std::make_shared<types::Trunk>(trunk));
  }
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<types::Trunk>>>::success(std::move(trunks)));
}

void MemoryDatastore::user_get(plugins::Executor on, std::string username, plugins::Handler<std::shared_ptr<types::User>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::User>>::success(_user_get(username)));
}

void MemoryDatastore::user_create(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_user_create(std::move(user)), "user_create"));
}

void MemoryDatastore::user_update(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_user_update(std::move(user)), "user_update"));
}

void MemoryDatastore::user_delete(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_user_delete(username), "user_delete"));
}

void MemoryDatastore::user_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::User>>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<types::User>>>::success(_user_list()));
}

void MemoryDatastore::session_create(plugins::Executor on, types::Session session, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_session_create(std::move(session)), "session_create"));
}

void MemoryDatastore::session_get(plugins::Executor on, std::string token_hash, plugins::Handler<std::shared_ptr<types::Session>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::Session>>::success(_session_get(token_hash)));
}

void MemoryDatastore::session_delete(plugins::Executor on, std::string token_hash, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_session_delete(token_hash), "session_delete"));
}

void MemoryDatastore::session_delete_for_user(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_session_delete_for_user(username), "session_delete_for_user"));
}

void MemoryDatastore::subscriber_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                                     plugins::Handler<std::shared_ptr<types::Subscriber>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::Subscriber>>::success(_subscriber_get(std::move(identity))));
}

void MemoryDatastore::subscriber_create(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_subscriber_create(std::move(subscriber)), "subscriber_create"));
}

void MemoryDatastore::subscriber_update(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_subscriber_update(std::move(subscriber)), "subscriber_update"));
}

void MemoryDatastore::subscriber_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_subscriber_delete(std::move(identity)), "subscriber_delete"));
}

void MemoryDatastore::subscriber_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Subscriber>>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<types::Subscriber>>>::success(_subscriber_list(realm_name)));
}

void MemoryDatastore::subscriber_register(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, types::Location binding,
                                          std::uint32_t expires_seconds, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_subscriber_register(subscriber, std::move(binding), expires_seconds), "subscriber_register"));
}

void MemoryDatastore::subscriber_unregister(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact,
                                            plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_subscriber_unregister(std::move(subscriber), std::move(contact)), "subscriber_unregister"));
}

void MemoryDatastore::location_list(plugins::Executor on, std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<types::Location>>::success(_location_list(subscriber_id)));
}

void MemoryDatastore::nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_nonce_create(nonce, expires_at), "nonce_create"));
}

void MemoryDatastore::nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<bool>::success(_nonce_check(std::move(nonce))));
}

void MemoryDatastore::call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_call_create(std::move(call)), "call_create"));
}

void MemoryDatastore::call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_call_update(std::move(call)), "call_update"));
}

void MemoryDatastore::call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<Call>>::success(_call_get(id)));
}

void MemoryDatastore::call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<Call>>>::success(_call_list()));
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

  // Sessions are pruned on their absolute expiry. Idle expiry is the caller's rule
  // (Session::has_expired).
  for (auto it = _sessions.begin(); it != _sessions.end();) {
    it = (it->second.expires_at != 0 && it->second.expires_at <= now) ? _sessions.erase(it) : std::next(it);
  }
}

}  // namespace athenasip::datastores
