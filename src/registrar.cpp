//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"
#include "transaction.h"
#include "session.h"

using namespace athenasip::databases;
using namespace athenasip::rtp;

namespace athenasip {

void Registrar::server_register(std::shared_ptr<servers::Server> server) { _servers.push_back(server); }

void Registrar::server_start_all(std::shared_ptr<SIPCore> core) {
  for (auto &server : _servers) server->start(core);
}

void Registrar::server_stop_all() {
  for (auto &server : _servers) server->stop();
}

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::databases::DB> db,
                     std::shared_ptr<events::EventSystem> events)
    : _logger(std::make_unique<LoggerScoped>("registrar", logger)),
      config(_config),
      _db(db),
      _events(events),
      _nonce_cache(std::make_shared<ExpirySet<std::string>>()) {}

bool Registrar::subscriber_exists(std::shared_ptr<SIPIdentity> identity) {
  std::shared_ptr<DBResult> res = _db->query("SELECT COUNT(*) FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() == 0) {
    _logger->warn("DB returned no rows on a COUNT() statement");
    return false;
  } else {
    return res->rows[0]->column_values[0]->as<int64_t>() == 1;
  }
}

std::shared_ptr<Subscriber> Registrar::subscriber_get(std::shared_ptr<SIPIdentity> identity) {
  std::shared_ptr<DBResult> res =
      _db->query("SELECT `id`,`name`,`h1` FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() == 0) return nullptr;

  auto obj = std::make_shared<Subscriber>();
  auto row = res->rows[0];
  obj->id = row->values["id"]->as<uint64_t>();
  obj->identity = identity;
  obj->h1 = row->values["h1"]->as<std::string>();
  return obj;
}

bool Registrar::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session) {
  std::shared_ptr<DBResult> res = _db->query("SELECT COUNT(*) FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?",
                                             {subscriber->id, contact->user, contact->realm, contact->port.value_or(0)});
  if (res && res->rows.size() == 0) {
    _logger->error("subscriber_register: COUNT(*) returned no rows");
    return false;
  };

  std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

  if (res->rows[0]->column_values[0]->as<int64_t>() == 0) {
    // Insert a row
    _db->query("INSERT INTO `location` (`subscriber_id`, `user`, `host`, `port`, `registered_at`, `nat`) VALUES (?,?,?,?,NOW(),?)",
               {subscriber->id, contact->user, contact->realm, contact->port.value_or(0), is_nat});
  } else {
    // Update the row
    _db->query("UPDATE `location` SET `registered_at` = NOW() WHERE `subscriber_id` = ?", {subscriber->id});
  }

  _sessions_by_subscriber[subscriber->id] = session;

  return true;
}

bool Registrar::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session) {
  _db->query("DELETE FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?",
             {subscriber->id, contact->user, contact->realm, contact->port.value_or(0)});

  _sessions_by_subscriber.erase(subscriber->id);

  return true;
}

std::shared_ptr<Session> Registrar::subscriber_get_session(std::shared_ptr<Subscriber> subscriber) { return _sessions_by_subscriber[subscriber->id]; }

// Sessions

bool Registrar::session_register(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  _sessions[endpoint] = session;
  _logger->debug("Registered Connection " + endpoint);
  return true;
}

bool Registrar::session_unregister(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  // _sessions.erase(endpoint);
  _logger->debug("Unregistered Connection " + endpoint);
  return true;
}

void Registrar::session_close_all() {
  // Close All Connections
  // for (const auto& pair : _sessions) pair.second->close();

  // Remove all connections
  std::unique_lock<std::shared_mutex> lock(_sessions_mutex);
  _sessions.clear();
  lock.unlock();
}

// Nonce

std::string Registrar::nonce_get() {
  std::array<unsigned char, 16> random_bytes;

  // Generate 128-bit (16-byte) secure random data
  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  // Get the current UNIX timestamp
  uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  // Concatenate random bytes and timestamp
  std::ostringstream raw_nonce_stream;
  raw_nonce_stream << Util::to_hex(random_bytes.data(), random_bytes.size()) << ":" << timestamp;
  std::string raw_nonce = raw_nonce_stream.str();

  // Compute HMAC-SHA256 using OpenSSL
  unsigned char hmac_result[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  HMAC(EVP_sha256(), config->sip_nonce_secret.c_str(), config->sip_nonce_secret.size(), reinterpret_cast<const unsigned char *>(raw_nonce.c_str()),
       raw_nonce.size(), hmac_result, &hmac_len);

  // Convert HMAC output to hex
  std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);

  // Generate expiry
  auto now = std::chrono::system_clock::now();
  std::chrono::seconds interval(config->sip_nonce_expiry);
  std::time_t expiresAt = std::chrono::system_clock::to_time_t(now + interval);

  // Cache result locally (timeout in ms)
  _nonce_cache->add(raw_nonce, config->sip_nonce_expiry * 1000);

  // Write to DB
  _db->query("INSERT INTO `nonce` (`id`, `expires_at`) VALUES (?,?)", {raw_nonce + ":" + hmac_hex, expiresAt});

  return raw_nonce + ":" + hmac_hex;
}

bool Registrar::nonce_check(std::string nonce) {
  // Check local cache
  if (_nonce_cache->contains(nonce)) return true;

  // Do DB
  std::shared_ptr<DBResult> res = _db->query("SELECT COUNT(*) FROM `nonce` WHERE `id` = ? AND `expires_at` > NOW()", {nonce});
  if (res && res->rows.size() == 0) {
    _logger->error("nonce_check: COUNT(*) returned no rows");
    return false;
  };

  return res->rows[0]->column_values[0]->as<int64_t>() == 1;
}

// Transactions
bool Registrar::transaction_register(std::shared_ptr<Transaction> transaction) {
  _transactions[transaction->id] = transaction;
  _events->publish("transaction.register", transaction->id);
  return true;
}

bool Registrar::transaction_unregister(std::string transactionId) {
  _transactions.erase(transactionId);
  _events->publish("transaction.unregister", transactionId);
  return true;
}

std::shared_ptr<Transaction> Registrar::transaction_get(std::string transactionId) {
  auto search = _transactions.find(transactionId);
  if (search == _transactions.end()) return nullptr;
  return search->second;
}

// Calls
bool Registrar::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;
  _events->publish("call.register", call->id);
  return true;
}

bool Registrar::call_unregister(std::string callId) {
  _calls.erase(callId);
  _events->publish("call.unregister", callId);
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
