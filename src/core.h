//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
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

#include "config.h"
#include "servers/server.h"
#include "datastores/datastore.h"
#include "events/event_system.h"
#include "rtp/rtp_relay_set.h"

#include "servers/server.h"
#include "api/admin_api.h"

#include "expiry_set.h"
#include "transaction.h"
#include "sip_message.h"
#include "call.h"

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"

using namespace athenasip::types;
using namespace athenasip::datastores;
using namespace athenasip::events;
using namespace athenasip::servers;
using namespace athenasip::rtp;
using namespace athenasip::api;

namespace athenasip {

class Core : public std::enable_shared_from_this<Core> {
 public:
  Core(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<Datastore> datastore,
                     std::shared_ptr<events::EventSystem> events);

  // Servers
  void server_register(std::shared_ptr<Server> server);
  void server_start_all();
  void server_stop_all();

  // Realms
  std::shared_ptr<Realm> realm_get_by_name(const std::string& realm);

  // Subscribers
  bool subscriber_exists(std::shared_ptr<SIPIdentity> identity);
  std::shared_ptr<Subscriber> subscriber_get(std::shared_ptr<SIPIdentity> identity);
  bool subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel);
  bool subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel);
  const std::shared_ptr<SIPUri> subscriber_get_location(std::shared_ptr<SIPIdentity> identity);

  // Channels
  bool channel_register(std::string endpoint, std::shared_ptr<Channel> channel);
  bool channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel);
  void channel_close_all();
  std::shared_ptr<Channel> subscriber_get_channel(std::shared_ptr<Subscriber> subscriber);

  // Nonce
  std::string nonce_create(std::shared_ptr<Realm> realm);
  bool nonce_check(std::string nonce);

  // Messages
  void process_message(std::shared_ptr<SIPMessage> message);

  // Transactions
  bool transaction_register(std::shared_ptr<Transaction> transaction);
  std::shared_ptr<Transaction> transaction_get(std::string transactionId);
  bool transaction_unregister(std::string transactionId);
  void transaction_end_all();

  // Calls
  bool call_register(std::shared_ptr<Call> call);
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
  std::shared_ptr<datastores::Datastore> datastore;
  std::shared_ptr<events::EventSystem> events;

 private:
  std::shared_ptr<loggers::Logger> _logger;

  std::unordered_map<std::string, std::shared_ptr<Channel>> _channels;
  std::unordered_map<uint64_t, std::shared_ptr<Channel>> _channels_by_subscriber;
  std::shared_mutex _channels_mutex;

  std::vector<std::shared_ptr<Server>> _servers;

  std::shared_ptr<rtp::RTPRelay> _rtprelay;
  std::shared_ptr<api::AdminAPI> _adminAPI;

  std::shared_ptr<ExpirySet<std::string>> _nonce_cache;

  std::unordered_map<std::string, std::shared_ptr<Transaction>> _transactions;
  std::shared_mutex _transactions_mutex;

  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;
};

}  // namespace athenasip