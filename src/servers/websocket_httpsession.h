//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <memory>
#include <string>
#include <utility>

#include "../channel.h"
#include "../core.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "websocket_connection.h"

namespace athenasip::servers {

namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

using namespace athenasip::loggers;

// RFC 7118 section 4: a SIP WebSocket client connects with an HTTP upgrade naming the
// "sip" subprotocol, and the server that accepts it must name it back. Everything before
// the upgrade is HTTP, which is why this is a session of its own rather than part of the
// connection: until the handshake completes there is nothing to hang a Channel off.
//
// Templated on the stream for the same reason the connection is - the TLS handshake has
// already happened by the time this is constructed, and from here on wss differs from ws
// only in what carries the bytes.
template <typename Stream>
class WebsocketHTTPSessionFor : public std::enable_shared_from_this<WebsocketHTTPSessionFor<Stream>> {
 public:
  WebsocketHTTPSessionFor(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, std::shared_ptr<Stream> stream, std::string transport)
      : _logger(std::make_shared<LoggerScoped>("websocket_http_channel", std::move(logger))),
        _core(std::move(core)),
        _stream(std::move(stream)),
        _transport(std::move(transport)) {}

  void start() { _do_read(); }

 private:
  void _do_read() {
    auto self = this->shared_from_this();
    http::async_read(*_stream, _buffer, _request, [self](boost::system::error_code ec, std::size_t bytes) { self->_on_read(ec, bytes); });
  }

  void _on_read(boost::system::error_code ec, std::size_t) {
    if (ec) {
      // Error reading request; close the socket.
      _close_socket();
      return;
    }

    _logger->debug(std::string(http::to_string(_request.method())) + " " + std::string(_request.target()));

    // A GET that asks to be upgraded, whatever path it asks on.
    //
    // RFC 7118 names no path, and every SIP over WebSocket client picks its own: "/ws"
    // is what Asterisk, Kamailio and FreeSWITCH serve and what a client is configured
    // with out of habit. This listener has a port to itself and serves nothing but SIP,
    // so there is nothing for a path to distinguish and refusing one is refusing a
    // client for a reason the RFC does not give. It used to insist on "/".
    if (_request.method() == http::verb::get && websocket::is_upgrade(_request)) {
      _do_upgrade();
      return;
    }

    // Send a 400 Bad Request response and close the connection.
    auto response = std::make_shared<http::response<http::string_body>>(http::status::bad_request, _request.version());
    response->set(http::field::content_type, "text/plain");
    response->body() = "Invalid request";
    response->prepare_payload();

    auto self = this->shared_from_this();
    http::async_write(*_stream, *response, [self, response](boost::system::error_code, std::size_t) { self->_close_socket(); });
  }

  void _do_upgrade() {
    // A new WebSocket stream, taking ownership of whatever carried the HTTP request.
    auto ws = std::make_unique<websocket::stream<Stream>>(std::move(*_stream));

    // RFC 7118 section 4: the server names the "sip" subprotocol back, or the client is
    // entitled to conclude the server does not speak SIP and close the connection.
    ws->set_option(websocket::stream_base::decorator([](websocket::response_type& response) { response.set(http::field::sec_websocket_protocol, "sip"); }));

    auto self = this->shared_from_this();
    ws->async_accept(_request, [self, ws = std::move(ws)](boost::system::error_code ec) mutable {
      // On error, stop and let it clean up.
      if (ec) return;

      auto connection = std::make_shared<WebsocketConnectionFor<Stream>>(std::move(ws), self->_transport);
      connection->start();

      // Start the actual channel with this connection
      auto channel = std::make_shared<athenasip::Channel>(self->_logger->base_logger(), self->_core, connection);
      channel->start();
    });
  }

  void _close_socket() {
    boost::system::error_code ec;
    auto& socket = boost::beast::get_lowest_layer(*_stream);
    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
  }

  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Core> _core;
  std::shared_ptr<Stream> _stream;
  std::string _transport;

  boost::beast::flat_buffer _buffer;
  http::request<http::string_body> _request;
};

using WebsocketHTTPSession = WebsocketHTTPSessionFor<tcp::socket>;
using WebsocketTLSHTTPSession = WebsocketHTTPSessionFor<boost::asio::ssl::stream<tcp::socket>>;

}  // namespace athenasip::servers
