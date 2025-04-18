//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <iostream>
#include <map>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>

#include "api/admin_api.h"
#include "call.h"
#include "config.h"
#include "databases/db.h"
#include "events/event_system.h"
#include "expiry_set.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "rtp/rtp_relay.h"
#include "servers/server.h"
#include "transaction.h"
#include "types/sip_identity.h"
#include "types/subscriber.h"

using namespace types;

namespace athenasip {

class Session;

class Registrar {
 public:
  Registrar(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Config> config, std::shared_ptr<databases::DB> db,
            std::shared_ptr<events::EventSystem> events);

  // Servers
  void server_register(std::shared_ptr<servers::Server> server);
  void server_start_all(std::shared_ptr<SIPCore> core);
  void server_stop_all();

  // Subscribers
  bool subscriber_exists(std::shared_ptr<SIPIdentity> identity);
  std::shared_ptr<Subscriber> subscriber_get(std::shared_ptr<SIPIdentity> identity);
  bool subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session);
  bool subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session);
  const std::shared_ptr<SIPUri> subscriber_get_location(std::shared_ptr<SIPIdentity> identity);

  // Sessions
  bool session_register(std::string endpoint, std::shared_ptr<Session> session);
  bool session_unregister(std::string endpoint, std::shared_ptr<Session> session);
  void session_close_all();
  std::shared_ptr<Session> subscriber_get_session(std::shared_ptr<Subscriber> subscriber);

  // Nonce
  std::string nonce_get();
  bool nonce_check(std::string nonce);

  // Transactions
  bool transaction_register(std::string transactionId, std::shared_ptr<Transaction> transaction);
  std::shared_ptr<Transaction> transaction_get(std::string transactionId);
  bool transaction_unregister(std::string transactionId);

  // Calls
  bool call_register(std::string callId, std::shared_ptr<Call> call);
  bool call_unregister(std::string callId);
  std::shared_ptr<Call> call_get(std::string callId);

  // RTPRelay
  void rtprelay_register(std::shared_ptr<rtp::RTPRelay> relay);
  void rtprelay_start();
  void rtprelay_stop();
  std::shared_ptr<rtp::RTPRelaySet> rtprelay_allocate();
  void rtprelay_release(std::shared_ptr<rtp::RTPRelaySet> relay);

  // Admin API
  void admin_register(std::shared_ptr<api::AdminAPI> adminAPI);
  void admin_start();
  void admin_stop();

  std::shared_ptr<Config> config;

 private:
  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<databases::DB> _db;
  std::shared_ptr<events::EventSystem> _events;

  std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
  std::unordered_map<uint64_t, std::shared_ptr<Session>> _sessions_by_subscriber;
  std::shared_mutex _sessions_mutex;

  std::vector<std::shared_ptr<servers::Server>> _servers;

  std::shared_ptr<rtp::RTPRelay> _rtprelay;
  std::shared_ptr<api::AdminAPI> _adminAPI;

  std::shared_ptr<ExpirySet<std::string>> _nonce_cache;

  std::unordered_map<std::string, std::shared_ptr<Transaction>> _transactions;
  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;
};

}  // namespace athenasip