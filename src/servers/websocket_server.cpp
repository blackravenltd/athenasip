//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "websocket_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/bind/bind.hpp>
#include <utility>

#include "tls_context.h"
#include "websocket_httpsession.h"

namespace athenasip {
namespace servers {

WebsocketServer::WebsocketServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port)
    : Server(std::make_shared<LoggerScoped>("wsu_server", logger), core),
      _acceptor(_io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(bind_address), port)),
      _ssl_context(boost::asio::ssl::context::tls_server) {}

bool WebsocketServer::set_certificates(std::string cert, std::string key) {
  if (!load_tls_certificates(_logger, _ssl_context, cert, key)) return false;

  _tls = true;
  return true;
}

void WebsocketServer::start() {
  _logger->debug("Starting...");
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + _acceptor.local_endpoint().address().to_string() + ":" + std::to_string(port()) + " (" + _scheme() + "://)");
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
    auto remote = socket->remote_endpoint();
    _logger->debug("Incoming Connection " + remote.address().to_string() + ":" + std::to_string(remote.port()));

    if (_tls) {
      _start_tls_session(std::move(socket));
    } else {
      // Straight to the HTTP upgrade. Until it completes there is nothing to hang a
      // Channel off.
      std::make_shared<WebsocketHTTPSession>(_logger->base_logger(), _core, std::move(socket), "ws")->start();
    }
  } else {
    _logger->error("Incoming Connection Accept Error: " + error.message());
  }

  boost::asio::post(_io_context, [this]() { start_accept(); });
}

void WebsocketServer::_start_tls_session(std::shared_ptr<boost::asio::ip::tcp::socket> socket) {
  auto stream = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*socket), _ssl_context);

  // Asynchronous, unlike the TLS SIP listener's blocking handshake: this one runs on the
  // listener's own thread, and a client that opens a connection and then says nothing
  // would otherwise stop every other client being accepted.
  stream->async_handshake(boost::asio::ssl::stream_base::server, [this, stream](const boost::system::error_code& ec) {
    if (ec) {
      _logger->info("TLS handshake failed: " + ec.message());
      return;
    }

    std::make_shared<WebsocketTLSHTTPSession>(_logger->base_logger(), _core, stream, "wss")->start();
  });
}

}  // namespace servers
}  // namespace athenasip
