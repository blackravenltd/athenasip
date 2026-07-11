//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "websocket_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/bind/bind.hpp>
#include <sstream>

#include "websocket_httpsession.h"

namespace athenasip {
namespace servers {

WebsocketServer::WebsocketServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port)
    : Server(std::make_shared<LoggerScoped>("wsu_server", logger), core),
      _port(port),
      _acceptor(_io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(bind_address), port)) {}

void WebsocketServer::start() {
  _logger->debug("Starting...");
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + _acceptor.local_endpoint().address().to_string() + ":" + std::to_string(_port) + " (ws://)");
    start_accept();
  });
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
}

void WebsocketServer::stop() {
  _logger->debug("Stopping...");
  if (_thread) {
    _io_context.stop();
    if (_thread->joinable()) {
      _thread->join();
      _thread.reset();
    }
    _logger->info("Stopped");
  } else {
    _logger->debug("Already Stopped");
  }
}

void WebsocketServer::start_accept() {
  auto new_socket = std::make_shared<boost::asio::ip::tcp::socket>(_io_context);
  _acceptor.async_accept(*new_socket, boost::bind(&WebsocketServer::_handle_accept, this, boost::asio::placeholders::error, new_socket));
}

void WebsocketServer::_handle_accept(const boost::system::error_code& error, std::shared_ptr<boost::asio::ip::tcp::socket> socket) {
  if (!error) {
    auto rm = socket->remote_endpoint();
    _logger->debug("Incoming Connection " + rm.address().to_string() + ":" + std::to_string(rm.port()));

    // Create a new HTTPSession to perform the HTTP upgrade.
    auto session = std::make_shared<WebsocketHTTPSession>(_logger->base_logger(), _core, socket);
    session->start();
  } else {
    _logger->error("Incoming Connection Accept Error: " + error.message());
  }
  boost::asio::post(_io_context, [this]() { start_accept(); });
}

}  // namespace servers
}  // namespace athenasip
