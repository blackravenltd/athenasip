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

#include "delayed_task.h"
#include "logger.h"
#include "logger_scoped.h"
#include "server.h"
#include "session.h"
#include "util.h"

namespace athenasip {

class TLSSession : public Session {
 public:
  TLSSession(std::shared_ptr<Logger> logger, std::string nonce_secret, std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection);

  void write(std::string message) override;
  void start() override;
  void close() override;

 private:
  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _connection;

  std::array<char, 65535> _read_buffer;
  std::string _buffer;

  void _schedule_async_read();
  bool _append_body();

  std::shared_ptr<DelayedTask<int>> _register_timeout;
};

}  // namespace athenasip
