//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "memory_datastore.h"

#include <boost/asio/post.hpp>
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

  // create is not update: an existing realm is a conflict, not an overwrite.
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
  return _realms.erase(realm_name) > 0;
}

std::vector<std::shared_ptr<types::Realm>> MemoryDatastore::_realm_list() {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Realm>> realms;
  realms.reserve(_realms.size());
  for (const auto& [name, realm] : _realms) realms.push_back(realm);

  return realms;
}

// A copy, not the record. A driver that goes to the network hands back what it
// deserialised and can do nothing else; this one must behave the same way, or a caller
// that changes a user it read without writing it back would change the roles on this
// node and not on a node running Redis. The user record is what authorises every
// request, and that is the worst place for two drivers to differ.
std::shared_ptr<types::User> MemoryDatastore::_user_get(const std::string& username) {
  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _users.find(types::User::normalise(username));
  return it == _users.end() ? nullptr : std::make_shared<types::User>(*it->second);
}

bool MemoryDatastore::_user_create(std::shared_ptr<types::User> user) {
  if (!user || user->username.empty()) return false;

  const auto key = user->key();

  std::lock_guard<std::mutex> lock(_mutex);

  // create is not update, and the key is case-folded, so "Tom" and "tom" are the same
  // conflict rather than two users nobody can tell apart at a login prompt.
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

  // A live token against a user that no longer exists is a session nobody can revoke.
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

// Writing a hash that is already held replaces it, which is how a session's last_seen_at
// moves: there is no session_update on the contract because a token hash is 32 bytes
// from a CSPRNG and does not collide by accident.
bool MemoryDatastore::_session_create(types::Session session) {
  if (session.token_hash.empty() || session.username.empty()) return false;

  // Issuing a session that is already dead is a caller bug, refused here for the same
  // reason nonce_create refuses an expired nonce - and Redis could not store it at all,
  // because SETEX has no non-positive expiry to give it.
  if (session.expires_at <= std::time(nullptr)) {
    _logger->warn("session_create: refusing to create already-expired session");
    return false;
  }

  // Filed under the same key the user is, so revoking by username finds them whatever
  // case the login was typed in.
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

// Gone either way, as the contract says: a hash that was not held is not a failure,
// because reporting the difference tells whoever asked whether the token they presented
// was a real one.
bool MemoryDatastore::_session_delete(const std::string& token_hash) {
  std::lock_guard<std::mutex> lock(_mutex);
  _sessions.erase(token_hash);
  return true;
}

// Nothing to revoke is not a failure: the caller asked for this user to hold no
// sessions, and it holds none.
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

std::shared_ptr<types::Account> MemoryDatastore::_account_get(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return nullptr;

  std::lock_guard<std::mutex> lock(_mutex);

  auto it = _accounts.find(_account_key(identity->uri->host, identity->uri->user));
  if (it == _accounts.end()) return nullptr;

  // Hand back the identity the caller asked with, as the Redis driver does, so the
  // returned account carries the tags of this request.
  // Every field the stored account has, not a chosen few: a copy that forgets one is a
  // credential that silently does not exist, which is exactly what happened to
  // ha1_sha256 the moment it was added.
  auto account = std::make_shared<types::Account>(*it->second);
  account->identity = std::move(identity);
  return account;
}

bool MemoryDatastore::_account_register(const std::shared_ptr<types::Account>& account, types::Location binding, std::uint32_t expires_seconds) {
  if (!account || !binding.contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  // The registrar negotiated this lifetime with the client and told the client about it
  // in the 200 OK, so the binding has to expire when it said it would. A caller that
  // asks for nothing gets the built-in default (RFC 3261 10.2.1).
  const std::time_t ttl = expires_seconds > 0 ? static_cast<std::time_t>(expires_seconds) : kDefaultRegistrationSeconds;

  const auto contact = binding.contact;
  const std::uint16_t port = contact->port.value_or(0);

  const auto now = std::time(nullptr);

  // The lifetime and the identity are the store's to settle; everything else on the
  // binding is what the caller knew and is kept as it was given.
  binding.account_id = account->id;
  binding.registered_at = now;
  binding.expires_at = now + ttl;
  binding.nat = Util::is_ipv4(contact->host) && Util::is_ipv4_private(contact->host);

  _locations[_location_key(account->id, contact->user, contact->host, port)] = std::move(binding);
  return true;
}

bool MemoryDatastore::_account_unregister(std::shared_ptr<types::Account> account, std::shared_ptr<types::SIPUri> contact) {
  if (!account || !contact) return false;

  std::lock_guard<std::mutex> lock(_mutex);

  const std::uint16_t port = contact->port.value_or(0);
  return _locations.erase(_location_key(account->id, contact->user, contact->host, port)) > 0;
}

std::vector<types::Location> MemoryDatastore::_location_list(std::uint64_t account_id) {
  std::lock_guard<std::mutex> lock(_mutex);
  _prune_expired();

  std::vector<types::Location> locations;
  for (const auto& [key, location] : _locations) {
    if (location.account_id == account_id) locations.push_back(location);
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

bool MemoryDatastore::_call_create(std::shared_ptr<Call> call) {
  if (!call || call->id.empty()) return false;

  std::lock_guard<std::mutex> lock(_mutex);
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

bool MemoryDatastore::_account_create(std::shared_ptr<types::Account> account) {
  if (!account || !account->identity || !account->identity->uri) return false;

  const auto key = _account_key(account->identity->uri->host, account->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_accounts.find(key) != _accounts.end()) return false;

  _accounts[key] = std::move(account);
  return true;
}

bool MemoryDatastore::_account_update(std::shared_ptr<types::Account> account) {
  if (!account || !account->identity || !account->identity->uri) return false;

  const auto key = _account_key(account->identity->uri->host, account->identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  if (_accounts.find(key) == _accounts.end()) return false;

  _accounts[key] = std::move(account);
  return true;
}

bool MemoryDatastore::_account_delete(std::shared_ptr<types::SIPIdentity> identity) {
  if (!identity || !identity->uri) return false;

  const auto key = _account_key(identity->uri->host, identity->uri->user);

  std::lock_guard<std::mutex> lock(_mutex);

  auto account = _accounts.find(key);
  if (account == _accounts.end()) return false;

  const auto account_id = account->second->id;
  _accounts.erase(account);

  // A deleted account keeps no bindings.
  for (auto it = _locations.begin(); it != _locations.end();) {
    it = (it->second.account_id == account_id) ? _locations.erase(it) : std::next(it);
  }

  return true;
}

std::vector<std::shared_ptr<types::Account>> MemoryDatastore::_account_list(const std::string& realm_name) {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<std::shared_ptr<types::Account>> accounts;
  for (const auto& [key, account] : _accounts) {
    if (realm_name.empty() || (account->identity && account->identity->uri && account->identity->uri->host == realm_name)) {
      accounts.push_back(account);
    }
  }

  return accounts;
}

// Every operation above answers at once; the contract is about where the handler runs,
// not about how long the work takes. These post it to the caller's executor so a
// caller is never re-entered from inside its own call, which is the one behaviour a
// driver that really does go to the network could not offer.
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

void MemoryDatastore::account_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                                  plugins::Handler<std::shared_ptr<types::Account>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::shared_ptr<types::Account>>::success(_account_get(std::move(identity))));
}

void MemoryDatastore::account_create(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_account_create(std::move(account)), "account_create"));
}

void MemoryDatastore::account_update(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_account_update(std::move(account)), "account_update"));
}

void MemoryDatastore::account_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_account_delete(std::move(identity)), "account_delete"));
}

void MemoryDatastore::account_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Account>>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::shared_ptr<types::Account>>>::success(_account_list(realm_name)));
}

void MemoryDatastore::account_register(plugins::Executor on, std::shared_ptr<types::Account> account, types::Location binding, std::uint32_t expires_seconds,
                                       plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_account_register(account, std::move(binding), expires_seconds), "account_register"));
}

void MemoryDatastore::account_unregister(plugins::Executor on, std::shared_ptr<types::Account> account, std::shared_ptr<types::SIPUri> contact,
                                         plugins::StatusHandler handler) {
  _complete(std::move(on), std::move(handler), _status(_account_unregister(std::move(account), std::move(contact)), "account_unregister"));
}

void MemoryDatastore::location_list(plugins::Executor on, std::uint64_t account_id, plugins::Handler<std::vector<types::Location>> handler) {
  _complete(std::move(on), std::move(handler), plugins::Result<std::vector<types::Location>>::success(_location_list(account_id)));
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

std::string MemoryDatastore::_account_key(const std::string& realm_name, const std::string& user) { return realm_name + ":" + user; }

std::string MemoryDatastore::_location_key(std::uint64_t account_id, const std::string& user, const std::string& host, std::uint16_t port) {
  return std::to_string(account_id) + ":" + user + ":" + host + ":" + std::to_string(port);
}

void MemoryDatastore::_prune_expired() {
  const auto now = std::time(nullptr);

  for (auto it = _locations.begin(); it != _locations.end();) {
    it = (it->second.expires_at <= now) ? _locations.erase(it) : std::next(it);
  }

  for (auto it = _nonces.begin(); it != _nonces.end();) {
    it = (it->second <= now) ? _nonces.erase(it) : std::next(it);
  }

  // A session's absolute expiry is written on the record and needs nothing else to
  // read, so the store keeps it. Idle expiry is not here: how long a session survives
  // unused is configuration the caller holds, and it asks Session::has_expired.
  for (auto it = _sessions.begin(); it != _sessions.end();) {
    it = (it->second.expires_at != 0 && it->second.expires_at <= now) ? _sessions.erase(it) : std::next(it);
  }
}

}  // namespace athenasip::datastores
