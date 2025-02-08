//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <memory>
#include <thread>
#include <unordered_map>

#include "logger.h"
#include "server.h"
#include "tls_session.h"

namespace athenasip {

class TLSServer : public Server {
 public:
  TLSServer(std::shared_ptr<Logger> logger, short port);
  ~TLSServer();

  virtual void set_certificates(std::string cert, std::string key);

  virtual void start();
  virtual void stop();

 private:
  void _handle_accept(const boost::system::error_code &error, std::shared_ptr<boost::asio::ip::tcp::socket> new_connection);
  void start_accept();

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::unordered_map<int, std::shared_ptr<TLSSession>> _connections;
  int next_connection_id_ = 0;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;
  boost::asio::ssl::context ctx;
  std::unique_ptr<Logger> _logger;
};

}  // namespace athenasip
