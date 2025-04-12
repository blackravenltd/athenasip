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
  explicit WebsocketConnection(std::unique_ptr<websocket::stream<tcp::socket>> ws);

  virtual bool start() override;

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;
  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;

  virtual boost::asio::ip::tcp::endpoint local_endpoint() override { return _local_endpoint; }
  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote_endpoint; }

  virtual bool is_open() override;
  virtual void shutdown() override;
  virtual void close() override;

  virtual std::string transport_name() const override;

 private:
  std::unique_ptr<websocket::stream<tcp::socket>> _ws;
  boost::asio::ip::tcp::endpoint _local_endpoint;
  boost::asio::ip::tcp::endpoint _remote_endpoint;
};

}  // namespace servers
}  // namespace athenasip
