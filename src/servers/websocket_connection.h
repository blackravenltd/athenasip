//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cstring>
#include <memory>

#include "connection.h"

namespace athenasip {
namespace servers {

namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

class WebsocketConnection : public Connection, public std::enable_shared_from_this<WebsocketConnection> {
 public:
  // NEW: Constructor now takes an already-upgraded WebSocket stream.
  explicit WebsocketConnection(std::unique_ptr<websocket::stream<tcp::socket>> ws);

  // With an already-upgraded connection, start() is a no-op.
  virtual bool start() override;

  // Reads an entire message into a temporary buffer then copies up to the user-supplied buffer capacity.
  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;

  // Writes the provided data as one complete WebSocket message.
  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;

  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override;
  virtual boost::asio::ip::tcp::endpoint local_endpoint() override;

  virtual bool is_open() override;
  virtual void shutdown() override;
  virtual void close() override;

  virtual std::string transport_name() const override;

 private:
  // Only the upgraded WebSocket stream is now stored.
  std::unique_ptr<websocket::stream<tcp::socket>> _ws;
};

}  // namespace servers
}  // namespace athenasip
