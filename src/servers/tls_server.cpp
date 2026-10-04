//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_server.h"

#include "../channel.h"
#include "tls_context.h"

using namespace athenasip;
using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip::servers {

TLSServer::TLSServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port)
    : Server(std::make_unique<LoggerScoped>("tls_server", logger), core),
      _port(port),
      _acceptor(_io_context, ip::tcp::endpoint(ip::make_address(bind_address), port)),
      ctx(ssl::context::tls_server) {}

bool TLSServer::set_certificates(std::string cert, std::string key) { return load_tls_certificates(_logger, ctx, cert, key); }

bool TLSServer::require_peer_certificates(const std::string& ca) { return athenasip::servers::require_peer_certificates(_logger, ctx, ca); }

void TLSServer::start() {
  _logger->debug("Starting...");

  // The first accept runs on the listener's own thread.
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + _acceptor.local_endpoint().address().to_string() + ":" + std::to_string(_acceptor.local_endpoint().port()) + " (tls://)");
    start_accept();
  });

  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
}

void TLSServer::stop() {
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

void TLSServer::start_accept() {
  auto new_connection = std::make_shared<ip::tcp::socket>(_io_context);
  _acceptor.async_accept(*new_connection, boost::bind(&TLSServer::_handle_accept, this, placeholders::error, new_connection));
}

// Each connection's handshake runs asynchronously under a deadline while the listener goes straight back to
// accepting, so a silent or failing peer cannot hold up or stop the listener.
void TLSServer::_handle_accept(const boost::system::error_code& error, std::shared_ptr<ip::tcp::socket> socket) {
  if (error) {
    _logger->error("Incoming Connection Accept Error: " + error.message());
  } else {
    auto ssl_socket = std::make_shared<ssl::stream<ip::tcp::socket>>(std::move(*socket), ctx);
    auto deadline = std::make_shared<boost::asio::steady_timer>(_io_context);

    deadline->expires_after(kHandshakeDeadline);
    deadline->async_wait([ssl_socket](const boost::system::error_code& ec) {
      if (ec == boost::asio::error::operation_aborted) return;
      boost::system::error_code ignored;
      ssl_socket->lowest_layer().close(ignored);
    });

    ssl_socket->async_handshake(ssl::stream_base::server, [this, ssl_socket, deadline](const boost::system::error_code& ec) {
      deadline->cancel();

      boost::system::error_code ignored;
      const auto remote = ssl_socket->lowest_layer().remote_endpoint(ignored);
      const auto name = remote.address().to_string() + ":" + std::to_string(remote.port());

      if (ec) {
        _logger->info("TLS handshake with " + name + " failed - " + ec.message());
        ssl_socket->lowest_layer().close(ignored);
        return;
      }

      std::shared_ptr<Connection> connection = std::make_shared<TLSConnection>(ssl_socket, true);
      auto channel = std::make_shared<Channel>(_logger->base_logger(), _core, connection);
      _logger->info("Incoming TLS Connection Accepted: " + name + (connection->peer_identity().empty() ? "" : ", node " + connection->peer_identity()));

      channel->start();
    });
  }

  start_accept();
}

}  // namespace athenasip::servers
