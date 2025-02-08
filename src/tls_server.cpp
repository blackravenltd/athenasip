//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_server.h"

#include <iostream>

#include "logger_scoped.h"
#include "tls_session.h"

using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip {

TLSServer::TLSServer(std::shared_ptr<Logger> logger, short port)
    : _port(port),
      _acceptor(_io_context, ip::tcp::endpoint(ip::tcp::v4(), port)),
      _logger(std::make_unique<LoggerScoped>("server", logger)),
      ctx(ssl::context::sslv23) {}

TLSServer::~TLSServer() { stop(); }

void TLSServer::set_certificates(std::string cert, std::string key) {
  _logger->info("Using Certificate PEM: " + cert);
  _logger->info("Using Key PEM: " + key);
  ctx.use_certificate_chain_file(cert);
  ctx.use_private_key_file(key, ssl::context::pem);
}

void TLSServer::start() {
  _logger->debug("Starting...");

  // Run the IO Context in our thread
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });

  // Do the first start_accept on that thread
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + std::to_string(_port) + " (TLS)");
    start_accept();
  });
}

void TLSServer::stop() {
  if (_thread) {
    _logger->debug("Stopping...");
    _io_context.stop();

    if (_thread->joinable()) {
      _thread->join();
      _thread.reset();
    }
    _logger->info("Stopped");
  }
}

void TLSServer::start_accept() {
  auto new_connection = std::make_shared<ip::tcp::socket>(_io_context);

  _acceptor.async_accept(*new_connection, boost::bind(&TLSServer::_handle_accept, this, placeholders::error, new_connection));
}

void TLSServer::_handle_accept(const boost::system::error_code& error, std::shared_ptr<ip::tcp::socket> new_connection) {
  boost::system::error_code ec;

  auto remote = new_connection->remote_endpoint();
  auto remote_addr = remote.address().to_string() + ":" + std::to_string(remote.port());

  _logger->debug("Incoming Connection: " + remote_addr);

  if (!error) {
    // Extract the socket from the shared_ptr and move it into the SSL stream
    auto ssl_socket = std::make_shared<ssl::stream<ip::tcp::socket>>(std::move(*new_connection), ctx);

    try {
      // SSL Handshake
      ssl_socket->handshake(ssl::stream_base::server);

      // Add the new connection to our map
      int id = next_connection_id_++;
      _connections[id] = std::make_shared<TLSSession>(_logger->base_logger(), ssl_socket);

      _logger->info("Incoming Connection Accepted: [" + std::to_string(id) + "] " + remote_addr);

    } catch (const std::exception& e) {
      _logger->info("Incoming Connection TLS Error: " + remote_addr + " " + e.what());

      ssl_socket = nullptr;
      new_connection->shutdown(ip::tcp::socket::shutdown_both, ec);
      new_connection->close(ec);
      new_connection = nullptr;
    }
  } else {
    _logger->info("Incoming Connection Accept Error: " + remote_addr + " " + ec.message());

    new_connection->shutdown(ip::tcp::socket::shutdown_both, ec);
    new_connection->close(ec);
    new_connection = nullptr;
  }

  // Start accepting next connection
  start_accept();
}

}  // namespace athenasip
