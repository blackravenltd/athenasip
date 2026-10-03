//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstdint>

#include "server.h"
#include "tls_connection.h"

using namespace athenasip::loggers;

namespace athenasip::servers {

class TLSServer : public Server {
 public:
  TLSServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port);

  void start() override;
  void stop() override;

  // The port actually being listened on. Not the configured one when that was zero and
  // the operating system chose, which is what a test binds and what a node logs.
  std::uint16_t port() const { return _acceptor.local_endpoint().port(); }

  bool set_certificates(std::string cert, std::string key);

  // Make this the inter-node listener: a peer has to show a certificate the cluster CA
  // signed (see tls_context.h).
  bool require_peer_certificates(const std::string& ca);

 protected:
  void _handle_accept(const boost::system::error_code& error, std::shared_ptr<boost::asio::ip::tcp::socket> new_connection);
  void start_accept();

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;
  boost::asio::ssl::context ctx;

  // How long a connection has to finish its handshake before it is closed.
  static constexpr std::chrono::seconds kHandshakeDeadline{10};
};

}  // namespace athenasip::servers
