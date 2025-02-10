//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <cstdint>
#include <fstream>
#include <memory>
#include <thread>

#include "logger.h"
#include "tls_server.h"

namespace athenasip {

class TLSSession : public std::enable_shared_from_this<TLSSession> {
 public:
  TLSSession(std::shared_ptr<Logger> logger, TLSServer* server, std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>);

  void close();

 private:
  std::unique_ptr<Logger> _logger;
  TLSServer* _server;

  std::unique_ptr<std::thread> _thread;
  std::atomic<bool> _running;

  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _connection;

  std::string remote_host;
  uint16_t remote_port;

  void _execute(int id);
  ssize_t _read_with_timeout(void* ptr, size_t len, uint32_t timeout_ms, boost::system::error_code& ec);

  void _ssl_close();

  boost::asio::io_context _rx_wait_context;
  boost::asio::steady_timer _rx_timer;
};

}  // namespace athenasip
