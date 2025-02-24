//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>

#include "call.h"
#include "databases/db.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "types/sip_identity.h"
#include "types/subscriber.h"
#include "rtp/rtp_relay.h"

using namespace athenasip::types;

namespace athenasip {

class Session;

class Registrar {
 public:
  Registrar(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<athenasip::databases::DB> db, std::shared_ptr<athenasip::rtp::RTPRelay> rtprelay);

  // SUbscribers
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

  // Calls
  bool call_register(std::string callId, std::shared_ptr<Call> call);
  bool call_unregister(std::string callId);
  std::shared_ptr<Call> call_get(std::string callId);

  // RTPRelay
  std::shared_ptr<rtp::RTPRelayPair> rtprelay_allocate();
  void rtprelay_release(std::shared_ptr<rtp::RTPRelayPair> relay);

 private:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<athenasip::databases::DB> _db;
  std::shared_ptr<athenasip::rtp::RTPRelay> _rtprelay;

  std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
  std::unordered_map<uint64_t, std::shared_ptr<Session>> _sessions_by_subscriber;
  std::shared_mutex _sessions_mutex;

  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;
};

}  // namespace athenasip