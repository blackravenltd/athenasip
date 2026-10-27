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
#include <algorithm>
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

// RFC 7118 section 4: the HTTP upgrade that precedes a SIP WebSocket. A session of its own because there is no
// connection to give a Channel until the upgrade completes. For wss the TLS handshake has already run.
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
      _close_socket();
      return;
    }

    _logger->debug(std::string(http::to_string(_request.method())) + " " + std::string(_request.target()));

    // A browser names the page that opened the socket in Origin (RFC 6455 4.1). With sip.websocket_allowed_origins
    // set, a page from anywhere else is refused, so another site cannot have a visitor's browser open flows here
    // (review finding 37). No Origin, as a phone sends, is not a browser and is let through.
    if (const auto origin = _request.find(http::field::origin); origin != _request.end() && _core && _core->config) {
      const auto& allowed = _core->config->sip_websocket_allowed_origins;
      const std::string given(origin->value());
      if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), given) == allowed.end()) {
        _logger->info("WebSocket upgrade from origin " + given + ", not one allowed - 403");
        auto refused = std::make_shared<http::response<http::string_body>>(http::status::forbidden, _request.version());
        refused->set(http::field::content_type, "text/plain");
        refused->body() = "Origin not allowed";
        refused->prepare_payload();
        auto self = this->shared_from_this();
        http::async_write(*_stream, *refused, [self, refused](boost::system::error_code, std::size_t) { self->_close_socket(); });
        return;
      }
    }

    // Any path is accepted: RFC 7118 names none, clients habitually use "/ws", and this listener serves only SIP.
    if (_request.method() == http::verb::get && websocket::is_upgrade(_request)) {
      _do_upgrade();
      return;
    }

    auto response = std::make_shared<http::response<http::string_body>>(http::status::bad_request, _request.version());
    response->set(http::field::content_type, "text/plain");
    response->body() = "Invalid request";
    response->prepare_payload();

    auto self = this->shared_from_this();
    http::async_write(*_stream, *response, [self, response](boost::system::error_code, std::size_t) { self->_close_socket(); });
  }

  void _do_upgrade() {
    auto ws = std::make_unique<websocket::stream<Stream>>(std::move(*_stream));

    // No message larger than a channel takes (Channel::kMaxMessageBytes): beast would otherwise gather up to 16 MB of
    // one before the channel's own limit could see it.
    ws->read_message_max(64 * 1024);

    // RFC 7118 section 4: the server must name the "sip" subprotocol back.
    ws->set_option(websocket::stream_base::decorator([](websocket::response_type& response) { response.set(http::field::sec_websocket_protocol, "sip"); }));

    auto self = this->shared_from_this();
    ws->async_accept(_request, [self, ws = std::move(ws)](boost::system::error_code ec) mutable {
      if (ec) return;

      auto connection = std::make_shared<WebsocketConnectionFor<Stream>>(std::move(ws), self->_transport);
      connection->start();

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
