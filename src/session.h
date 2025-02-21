//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "delayed_task.h"
#include "expiry_set.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "sdp.h"
#include "servers/connection.h"
#include "session.h"
#include "sip_header.h"
#include "sip_message.h"
#include "types/authorization.h"
#include "types/sip_identity.h"
#include "types/sip_uri.h"
#include "types/subscriber.h"
#include "util.h"

using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(std::shared_ptr<Logger> logger, std::string nonce_secret, std::shared_ptr<Connection> connection);

  enum State {
    Normal,
    InCall,
    Bye,
    Error,
    Closing,
    Closed,
  } uint8_t;

  void write(std::string message);
  void start();
  void close();

  using StartCloseFn = std::function<bool(std::string, std::shared_ptr<Session>)>;
  using AuthenticateFn = std::function<std::shared_ptr<Subscriber>(std::shared_ptr<SIPIdentity>, std::shared_ptr<Session>)>;
  using RegisterLocationFn = std::function<bool(std::shared_ptr<Subscriber>, std::shared_ptr<SIPUri>, std::shared_ptr<Session>)>;
  using GetSubscriberSessionFn = std::function<std::shared_ptr<Session>(std::shared_ptr<Subscriber>)>;

  State state = State::Normal;

  // The other session when Caller/Callee
  std::shared_ptr<Session> other_session;

  void send(std::shared_ptr<SIPMessage> message);

  void on_start(StartCloseFn callback);
  void on_close(StartCloseFn callback);
  void on_authenticate(AuthenticateFn callback);
  void on_register_location(RegisterLocationFn callback);
  void on_unregister_location(RegisterLocationFn callback);
  void on_get_subscriber_session(GetSubscriberSessionFn callback);

 protected:
  std::shared_ptr<Logger> _logger;

  std::shared_ptr<Connection> _connection;
  std::shared_ptr<SIPMessage> _request;
  std::shared_ptr<SIPMessage> _response;

  std::string _nonce_secret;
  std::shared_ptr<ExpirySet<std::string>> _nonces;
  std::string _generate_nonce() const;

  StartCloseFn _on_start;
  StartCloseFn _on_close;
  AuthenticateFn _on_authenticate;
  RegisterLocationFn _on_register_location;
  RegisterLocationFn _on_unregister_location;
  GetSubscriberSessionFn _on_get_subscriber_session;

  std::shared_ptr<Subscriber> _subscriber;
  std::shared_ptr<SIPUri> _contact;

  void _process_message_register();
  void _process_message_invite();
  void _send_auth_challenge();

  void _process_buffer();
  void _process_message();
  void _process_message_normal();
  void _process_message_incall();
  void _process_message_bye();

  void _send(uint16_t code, std::string message);
  void _send_close(uint16_t code, std::string message);

  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  void _schedule_async_read();
  bool _append_body();

  std::shared_ptr<DelayedTask<int>> _register_timeout;
};

}  // namespace athenasip
