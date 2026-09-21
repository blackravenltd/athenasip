//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "connection.h"

namespace athenasip {
namespace servers {

namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

// RFC 7118. A SIP WebSocket connection, over a plain TCP socket or over TLS.
//
// The next layer is a template parameter rather than two classes, because everything
// below the websocket framing is identical: a browser's ws:// and its wss:// differ in
// what carries the frames and in nothing this class does with them. The transport name
// is carried rather than deduced, because it is the one thing that does differ and the
// layers above route on it.
template <typename NextLayer>
class WebsocketConnectionFor : public Connection, public std::enable_shared_from_this<WebsocketConnectionFor<NextLayer>> {
 public:
  using Stream = websocket::stream<NextLayer>;

  WebsocketConnectionFor(std::unique_ptr<Stream> ws, std::string transport) : _ws(std::move(ws)), _transport(std::move(transport)) {}

  bool start() override {
    auto& socket = boost::beast::get_lowest_layer(*_ws);
    _local_endpoint = socket.local_endpoint();
    _remote_endpoint = socket.remote_endpoint();
    return true;
  }

  boost::asio::any_io_executor executor() override { return _ws->get_executor(); }

  void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    auto self = this->shared_from_this();
    _ws->async_read_some(buffer, [self, handler](boost::system::error_code ec, std::size_t length) {
      // Rationalise close error code
      if (ec == websocket::error::closed || ec == boost::asio::error::not_connected) ec = boost::asio::error::eof;
      handler(ec, length);
    });
  }

  void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    auto self = this->shared_from_this();
    _ws->async_write_some(true, buffer, [self, handler](boost::system::error_code ec, std::size_t length) {
      // Rationalise close error code
      if (ec == websocket::error::closed || ec == boost::asio::error::not_connected) ec = boost::asio::error::eof;
      handler(ec, length);
    });
  }

  boost::asio::ip::tcp::endpoint local_endpoint() override { return _local_endpoint; }
  boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote_endpoint; }

  bool is_open() override { return _ws && _ws->is_open(); }

  bool is_reliable() override { return true; }

  void shutdown() override {
    boost::system::error_code ec;
    if (_ws) boost::beast::get_lowest_layer(*_ws).shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
  }

  void close() override {
    boost::system::error_code ec;
    if (_ws) _ws->close(websocket::close_code::normal, ec);
  }

  std::string transport_name() const override { return _transport; }

 private:
  std::unique_ptr<Stream> _ws;
  std::string _transport;
  boost::asio::ip::tcp::endpoint _local_endpoint;
  boost::asio::ip::tcp::endpoint _remote_endpoint;
};

// ws:// over a plain socket, and wss:// over TLS. Browsers need the second: a page
// served over https may not open an insecure WebSocket, so wss is not a hardening option
// for a web client but the only way in.
using WebsocketConnection = WebsocketConnectionFor<tcp::socket>;
using WebsocketTLSConnection = WebsocketConnectionFor<boost::asio::ssl::stream<tcp::socket>>;

}  // namespace servers
}  // namespace athenasip
