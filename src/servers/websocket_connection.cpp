//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
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

namespace athenasip {
namespace servers {

WebsocketConnection::WebsocketConnection(std::unique_ptr<websocket::stream<tcp::socket>> ws) : _ws(std::move(ws)) {}

bool WebsocketConnection::start() {
  // With an already-upgraded connection, there is nothing else to do.
  return true;
}

void WebsocketConnection::async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  if (!_ws) {
    // If the connection was not upgraded properly, post a not_connected error.
    boost::asio::post(_ws->get_executor(), [handler]() { handler(boost::asio::error::not_connected, 0); });
    return;
  }
  // Since Boost.Beast reads entire messages, use a temporary flat_buffer.
  auto temp_buffer = std::make_shared<boost::beast::flat_buffer>();
  auto self = shared_from_this();
  _ws->async_read(*temp_buffer, [this, self, temp_buffer, buffer, handler](boost::system::error_code ec, std::size_t) {
    if (!ec) {
      // Copy up to the requested number of bytes.
      auto data = temp_buffer->data();
      std::size_t available = temp_buffer->size();
      std::size_t to_copy = std::min(boost::asio::buffer_size(buffer), available);
      std::memcpy(buffer.data(), data.data(), to_copy);
      temp_buffer->consume(to_copy);
      handler(ec, to_copy);
    } else {
      handler(ec, 0);
    }
  });
}

void WebsocketConnection::async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  if (!_ws) {
    boost::asio::post(_ws->get_executor(), [handler]() { handler(boost::asio::error::not_connected, 0); });
    return;
  }
  // Write the entire buffer as one complete WebSocket message.
  _ws->async_write(boost::asio::buffer(buffer), handler);
}

boost::asio::ip::tcp::endpoint WebsocketConnection::remote_endpoint() {
  if (_ws) return _ws->next_layer().remote_endpoint();
  return boost::asio::ip::tcp::endpoint();
}

boost::asio::ip::tcp::endpoint WebsocketConnection::local_endpoint() {
  if (_ws) return _ws->next_layer().local_endpoint();
  return boost::asio::ip::tcp::endpoint();
}

bool WebsocketConnection::is_open() {
  if (_ws) return _ws->next_layer().is_open();
  return false;
}

void WebsocketConnection::shutdown() {
  boost::system::error_code ec;
  if (_ws) _ws->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
}

void WebsocketConnection::close() {
  boost::system::error_code ec;
  if (_ws) _ws->close(boost::beast::websocket::close_code::normal);
}

std::string WebsocketConnection::transport_name() const { return "websocket"; }

}  // namespace servers
}  // namespace athenasip
