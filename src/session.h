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

#include "logger.h"
#include "logger_scoped.h"
#include "sip_header.h"
#include "sip_message.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "types/authorization.h"
#include "util.h"

namespace athenasip {

class Session : public std::enable_shared_from_this<Session> {
 public:
  using EventFn = std::function<bool(std::string, std::shared_ptr<Session>)>;

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

  void on_register(EventFn callback);
  void on_unregister(EventFn callback);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<SIPMessage> _current_message;
  std::string _nonce_secret;

  EventFn _on_register;
  EventFn _on_unregister;

  void _process_message();
  void _process_message_initial();
  void _process_message_challenged();

  std::string _generate_nonce();
};

}  // namespace athenasip
