//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tcp_server.h"

using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip::servers {

TCPServer::TCPServer(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string nonce_secret, short port)
    : Server(std::make_unique<LoggerScoped>("tcp_server", logger), registrar, nonce_secret),
      _port(port),
      _acceptor(_io_context, ip::tcp::endpoint(ip::tcp::v4(), port)) {}

void TCPServer::start() {
  _logger->debug("Starting...");

  // Do the first start_accept on that thread
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + std::to_string(_port) + " (TCP)");
    start_accept();
  });

  // Run the IO Context in our thread
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
}

void TCPServer::stop() {
  _logger->debug("Stopping...");

  // Stop Thread
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

void TCPServer::start_accept() {
  auto new_connection = std::make_shared<ip::tcp::socket>(_io_context);
  _acceptor.async_accept(*new_connection, boost::bind(&TCPServer::_handle_accept, this, placeholders::error, new_connection));
}

void TCPServer::_handle_accept(const boost::system::error_code& error, std::shared_ptr<ip::tcp::socket> socket) {
  boost::system::error_code ec;

  if (!error) {
    std::shared_ptr<Connection> new_connection = std::make_shared<TCPConnection>(socket);

    _logger->debug("Starting TCP Connection: " + new_connection->remote_endpoint_name());

    if (!new_connection->start()) {
      _logger->info("Incoming TCP Connection Error: " + new_connection->remote_endpoint_name());

      new_connection->shutdown();
      new_connection->close();
      return;
    }

    // Create TLSSession from connection
    auto new_session = std::make_shared<Session>(_logger->base_logger(), nonce_secret, new_connection);
    _logger->info("Incoming TCP Connection Accepted: " + new_connection->remote_endpoint_name());

    // Setup Session Events
    set_session_events(new_session);

    new_session->start();

  } else {
    _logger->error("Incoming TCP Connection Accept Error: " + ec.message());
  }

  // Start accepting next connection
  boost::asio::post(_io_context, [this]() { start_accept(); });
}

}  // namespace athenasip::servers
