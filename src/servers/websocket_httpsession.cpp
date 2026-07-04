//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "websocket_httpsession.h"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include "../channel.h"
#include "websocket_connection.h"

namespace athenasip {
namespace servers {

void WebsocketHTTPSession::start() { do_read(); }

void WebsocketHTTPSession::do_read() {
  auto self = shared_from_this();
  http::async_read(*_socket, _buffer, _req, [this, self](boost::system::error_code ec, std::size_t bytes_transferred) { on_read(ec, bytes_transferred); });
}

void WebsocketHTTPSession::on_read(boost::system::error_code ec, std::size_t) {
  if (ec) {
    // Error reading request; close the socket.
    boost::system::error_code ec_close;
    _socket->shutdown(tcp::socket::shutdown_both, ec_close);
    _socket->close();
    return;
  }

  _logger->debug(std::string(http::to_string(_req.method())) + " " + std::string(_req.target()));

  // Validate that the request is a GET for "/" and that it is a WebSocket upgrade.
  if (_req.method() == http::verb::get && _req.target() == "/" && websocket::is_upgrade(_req)) {
    do_upgrade();
    return;
  }

  // Send a 400 Bad Request response and close the connection.
  auto res = std::make_shared<http::response<http::string_body>>(http::status::bad_request, _req.version());
  res->set(http::field::content_type, "text/plain");
  res->body() = "Invalid request";
  res->prepare_payload();
  auto self = shared_from_this();

  http::async_write(*_socket, *res, [this, self, res](boost::system::error_code, std::size_t) {
    boost::system::error_code ec_shutdown;
    _socket->shutdown(tcp::socket::shutdown_send, ec_shutdown);
    _socket->close();
  });
}

void WebsocketHTTPSession::do_upgrade() {
  // Create a new WebSocket stream by transferring ownership of the TCP socket.
  auto ws = std::make_unique<websocket::stream<tcp::socket>>(std::move(*_socket));

  // Set a decorator to ensure that the "sip" subprotocol is included in the handshake.
  ws->set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
    // Advertise the "sip" subprotocol.
    res.set(http::field::sec_websocket_protocol, "sip");
  }));

  // Perform the asynchronous WebSocket handshake using the HTTP request.
  auto self = shared_from_this();
  ws->async_accept(_req, [this, ws = std::move(ws), self](boost::system::error_code ec) mutable {
    // On error, stop and let it clean up.
    if (ec) return;

    // On successful upgrade, create a WebsocketConnection using the upgraded stream.
    auto connection = std::make_shared<WebsocketConnection>(std::move(ws));
    connection->start();

    // Start the actual channel with this connection
    auto channel = std::make_shared<athenasip::Channel>(_logger->base_logger(), _core, connection);
    channel->start();
  });
}

}  // namespace servers
}  // namespace athenasip
