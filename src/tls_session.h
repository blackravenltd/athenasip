//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
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

#include "authorization_header.h"
#include "delayed_task.h"
#include "logger.h"
#include "logger_scoped.h"
#include "sip_header.h"
#include "sip_message.h"
#include "tls_server.h"
#include "util.h"

namespace athenasip {

class TLSServer;

class TLSSession : public std::enable_shared_from_this<TLSSession> {
 public:
  TLSSession(std::shared_ptr<Logger> logger, std::shared_ptr<TLSServer> server, std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>);

  void write(std::string message);
  void close();

  std::string remote_endpoint;

  void _start();

  enum State {
    Initial,
    Challenged,
    Registered,
    Error,
    Closing,
  } uint8_t;

  State state = State::Initial;

 private:
  std::shared_ptr<TLSServer> _server;

  std::unique_ptr<Logger> _logger;
  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _connection;

  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  std::shared_ptr<SIPMessage> _current_message;

  void _schedule_async_read();
  ssize_t _read_async();
  bool _append_body();

  void _process_message();
  void _process_message_initial();
  void _process_message_challenged();

  std::string _generate_nonce();

  std::shared_ptr<DelayedTask<int>> _register_timeout;
};

}  // namespace athenasip
