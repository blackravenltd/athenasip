//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "expiry_set.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "logger.h"
#include "logger_scoped.h"
#include "sip_header.h"
#include "sip_message.h"
#include "types/authorization.h"
#include "types/sip_identity.h"
#include "util.h"

using namespace athenasip::types;

namespace athenasip {

class Session : public std::enable_shared_from_this<Session> {
 public:
  using StartCloseFn = std::function<bool(std::string, std::shared_ptr<Session>)>;
  using AuthenticateFn = std::function<std::optional<std::string>(std::shared_ptr<SIPIdentity> identity, std::shared_ptr<Session>)>;

  Session(std::shared_ptr<Logger> logger, std::string nonce_secret);

  virtual void write(std::string message) = 0;
  virtual void start() = 0;
  virtual void close() = 0;

  enum State {
    Initial,
    Challenged,
    Registered,
    Error,
    Closing,
  } uint8_t;

  State state = State::Initial;
  std::string remote_endpoint;

  void on_start(StartCloseFn callback);
  void on_close(StartCloseFn callback);
  void on_authenticate(AuthenticateFn callback);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<SIPMessage> _request;
  std::shared_ptr<SIPMessage> _response;
  std::string _nonce_secret;

  std::shared_ptr<ExpirySet<std::string>> _nonces;

  StartCloseFn _on_start;
  StartCloseFn _on_close;
  AuthenticateFn _on_authenticate;

  void _process_message();
  void _process_message_initial();
  void _process_message_challenged();

  void _send(uint16_t code, std::string message);
  void _send_close(uint16_t code, std::string message);

  std::string _generate_nonce() const;
};

}  // namespace athenasip
