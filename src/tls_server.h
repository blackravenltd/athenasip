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
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "logger.h"
#include "logger_scoped.h"
#include "registrar.h"
#include "tls_session.h"

namespace athenasip {

class TLSSession;

class TLSServer : public std::enable_shared_from_this<TLSServer> {
 public:
  TLSServer(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, short port);

  void set_certificates(std::string cert, std::string key);

  void start();
  void stop();

  void register_connection(std::shared_ptr<TLSSession> session);
  void unregister_connection(std::shared_ptr<TLSSession> session);

  std::string nonce_secret = "testing123";

 private:
  void _handle_accept(const boost::system::error_code &error, std::shared_ptr<boost::asio::ip::tcp::socket> new_connection);
  void start_accept();

  std::shared_ptr<Registrar> _registrar;

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;
  boost::asio::ssl::context ctx;
  std::unique_ptr<Logger> _logger;

  std::unordered_map<std::string, std::shared_ptr<TLSSession>> _connections;
  std::shared_mutex _connections_mtx;
};

}  // namespace athenasip
