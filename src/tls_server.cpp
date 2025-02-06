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
      _logger(std::make_shared<LoggerScoped>("server", logger)),
      ctx(ssl::context::tlsv13) {
  start_accept();
}

TLSServer::~TLSServer() { stop(); }

void TLSServer::set_certificates(std::string cert, std::string key) {
  _logger->info("Using Certificate PEM: "+cert);
  _logger->info("Using Key PEM: "+key);
  ctx.use_certificate_chain_file(cert);
  ctx.use_private_key_file(key, ssl::context::pem);
}

void TLSServer::start() {
  _logger->debug("Starting...");
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
  _logger->info("Listening on " + std::to_string(_port)+ " (TLS)");
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
  if (!error) {
    // Extract the socket from the shared_ptr and move it into the SSL stream
    auto ssl_socket = std::make_shared<ssl::stream<ip::tcp::socket>>(std::move(*new_connection), ctx);

    try {
      ssl_socket->handshake(ssl::stream_base::server);

      // Add the new connection to the map
      int id = next_connection_id_++;
      _connections[id] = std::make_shared<TLSSession>(_logger, ssl_socket);

      _logger->info("New Connection #" + std::to_string(id) + " (" + ssl_socket->next_layer().remote_endpoint().address().to_string() + ")");

    } catch (const std::exception& e) {
      std::cerr << "TLS error: " << e.what() << std::endl;
    }

  } else {
    _logger->error("Error accepting new connection");

    boost::system::error_code ec;
    new_connection->shutdown(ip::tcp::socket::shutdown_both, ec);
    new_connection->close(ec);
  }

  // Start accepting another connection
  start_accept();
}

}  // namespace athenasip
