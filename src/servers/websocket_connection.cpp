//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "websocket_connection.h"

#include <algorithm>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstring>
#include <iostream>

namespace athenasip {
namespace servers {

WebsocketConnection::WebsocketConnection(std::unique_ptr<websocket::stream<tcp::socket>> ws) : _ws(std::move(ws)) {}

bool WebsocketConnection::start() {
  _local_endpoint = _ws->next_layer().local_endpoint();
  _remote_endpoint = _ws->next_layer().remote_endpoint();
  return true;
}

void WebsocketConnection::async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  auto self = shared_from_this();
  _ws->async_read_some(buffer, [this, self, handler](boost::system::error_code ec, std::size_t length) {
    // Rationalise close error code
    if (ec == websocket::error::closed || ec == boost::asio::error::not_connected) ec = boost::asio::error::eof;
    handler(ec, length);
  });
}

void WebsocketConnection::async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  auto self = shared_from_this();
  _ws->async_write_some(true, buffer, [this, self, handler](boost::system::error_code ec, std::size_t length) {
    // Rationalise close error code
    if (ec == websocket::error::closed || ec == boost::asio::error::not_connected) ec = boost::asio::error::eof;
    handler(ec, length);
  });
}

bool WebsocketConnection::is_open() {
  if (_ws) return _ws->is_open();
  return false;
}

void WebsocketConnection::shutdown() {
  boost::system::error_code ec;
  if (_ws) _ws->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
}

void WebsocketConnection::close() {
  boost::system::error_code ec;
  if (_ws) _ws->close(boost::beast::websocket::close_code::normal, ec);
}

std::string WebsocketConnection::transport_name() const { return "ws"; }

}  // namespace servers
}  // namespace athenasip
