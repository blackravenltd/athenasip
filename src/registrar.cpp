//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

#include "channel.h"
#include "transaction.h"

using namespace athenasip::datastores;
using namespace athenasip::rtp;

namespace athenasip {

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::datastores::Datastore> datastore,
                     std::shared_ptr<events::EventSystem> events)
    : _logger(std::make_unique<LoggerScoped>("registrar", logger)),
      config(_config),
      _datastore(datastore),
      _events(events),
      _nonce_cache(std::make_shared<ExpirySet<std::string>>()) {}

void Registrar::server_register(std::shared_ptr<servers::Server> server) { _servers.push_back(server); }

void Registrar::server_start_all(std::shared_ptr<SIPCore> core) {
  for (auto& server : _servers) server->start(core);
}

void Registrar::server_stop_all() {
  for (auto& server : _servers) server->stop();
}

// Realms
std::shared_ptr<Realm> Registrar::realm_get_by_name(const std::string& realm_name) {
  return _datastore->realm_get_by_name(realm_name);
};

// Events
void Registrar::event_publish(const std::string& event, const std::string& payload) { _events->publish(event, payload); }

// Subscribers
std::shared_ptr<Subscriber> Registrar::subscriber_get(std::shared_ptr<SIPIdentity> identity) {
  return _datastore->subscriber_get(identity);
}

bool Registrar::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {
  if(_datastore->subscriber_register(subscriber, contact)) {
    _channels_by_subscriber[subscriber->id] = channel;
    return true;
  }

  _logger->error("Cannot register subscriber identity "+ subscriber->identity->to_string()+" - datastore failure");
  return false;
}

bool Registrar::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {
  if(_datastore->subscriber_unregister(subscriber, contact)) {
    _channels_by_subscriber.erase(subscriber->id);
    return true;
  }

    _logger->error("Cannot unregister subscriber identity "+ subscriber->identity->to_string()+" - datastore failure");
  return false;
}

std::shared_ptr<Channel> Registrar::subscriber_get_channel(std::shared_ptr<Subscriber> subscriber) { return _channels_by_subscriber[subscriber->id]; }

// Channels

bool Registrar::channel_register(std::string endpoint, std::shared_ptr<Channel> channel) {
  std::lock_guard<std::shared_mutex> lock(_channels_mutex);
  _channels[endpoint] = channel;
  _logger->debug("Registered Channel " + endpoint);
  return true;
}

bool Registrar::channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel) {
  std::lock_guard<std::shared_mutex> lock(_channels_mutex);
  // _channels.erase(endpoint);
  _logger->debug("Unregistered Channel " + endpoint);
  return true;
}

void Registrar::channel_close_all() {
  // Close All Channels
  for (const auto& pair : _channels) pair.second->close();

  // Remove all Channels
  std::unique_lock<std::shared_mutex> lock(_channels_mutex);
  _channels.clear();
}

// Nonce

std::string Registrar::nonce_create(std::shared_ptr<Realm> realm) {

  std::array<unsigned char, 16> random_bytes;

  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  const std::string random_hex = Util::to_hex(random_bytes.data(), random_bytes.size());

  // Public - This is visible to the client.
  const std::string raw_nonce = std::to_string(realm->id) + ":" + random_hex + ":" + std::to_string(timestamp);

  // Sign the public nonce material using the realm secret.
  unsigned char hmac_result[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  HMAC(EVP_sha256(), realm->nonce_secret.data(), static_cast<int>(realm->nonce_secret.size()), reinterpret_cast<const unsigned char*>(raw_nonce.data()),
       raw_nonce.size(), hmac_result, &hmac_len);

  const std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);
  const std::string nonce = raw_nonce + ":" + hmac_hex;

  const auto expires_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::seconds(realm->nonce_expiry));

  if(_datastore->nonce_create(nonce, expires_at)) {
    // Cache the actual nonce for this node
    _nonce_cache->add(nonce, realm->nonce_expiry * 1000);
    return nonce;
  }

  throw std::runtime_error("Failed to generate nonce - datastore error");
}

bool Registrar::nonce_check(std::string nonce) {
  return _datastore->nonce_check(nonce);
}

// Transactions

bool Registrar::transaction_register(std::shared_ptr<Transaction> transaction) {
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);

  _transactions[transaction->id] = transaction;
  event_publish("transaction.register", transaction->id);
  return true;
}

bool Registrar::transaction_unregister(std::string transactionId) {
  auto transaction = transaction_get(transactionId);

  if (!transaction) return false;

  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);
  _transactions.erase(transactionId);
  event_publish("transaction.unregister", transactionId);
  return true;
}

std::shared_ptr<Transaction> Registrar::transaction_get(std::string transactionId) {
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);

  auto search = _transactions.find(transactionId);
  if (search == _transactions.end()) return nullptr;
  return search->second;
}

void Registrar::transaction_end_all() {
  // End all transactions
  for (const auto& pair : _transactions) pair.second->end();

  // Remove all transactions
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);
  _transactions.clear();
}

// Calls
bool Registrar::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;
  event_publish("call.register", call->id);
  return true;
}

bool Registrar::call_unregister(std::string callId) {
  _calls.erase(callId);
  event_publish("call.unregister", callId);
  return true;
}

std::shared_ptr<Call> Registrar::call_get(std::string callId) {
  auto search = _calls.find(callId);
  if (search == _calls.end()) return nullptr;
  return search->second;
}

// RTP Relays

void Registrar::rtprelay_register(std::shared_ptr<rtp::RTPRelay> relay) { _rtprelay = relay; }

void Registrar::rtprelay_start() {
  if (_rtprelay) _rtprelay->start();
}
void Registrar::rtprelay_stop() {
  if (_rtprelay) _rtprelay->stop();
}

void Registrar::admin_register(std::shared_ptr<api::AdminAPI> adminAPI) { _adminAPI = adminAPI; }

void Registrar::admin_start() {
  if (_adminAPI) _adminAPI->start();
}

void Registrar::admin_stop() {
  if (_adminAPI) _adminAPI->stop();
}

std::shared_ptr<RTPRelaySet> Registrar::rtprelay_allocate() {
  if (!_rtprelay) return nullptr;
  return _rtprelay->allocate_relay_set();
}

void Registrar::rtprelay_release(std::shared_ptr<RTPRelaySet> relay) {
  if (!_rtprelay) return;
  _rtprelay->release_relay_set(relay);
}

}  // namespace athenasip
