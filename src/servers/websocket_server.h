//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <thread>

#include "server.h"
#include "websocket_connection.h"

namespace athenasip {
namespace servers {

class WebsocketServer : public Server {
 public:
  WebsocketServer(std::shared_ptr<Logger> logger, std::shared_ptr<SIPCore> core, const std::string &bind_address, short port);
  void start() override;
  void stop() override;

 protected:
  void start_accept();
  void _handle_accept(const boost::system::error_code &error, std::shared_ptr<boost::asio::ip::tcp::socket> socket);

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;
};

}  // namespace servers
}  // namespace athenasip
