//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstddef>
#include <functional>
#include <memory>

#include "connection.h"

using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip::servers {

class TCPConnection : public Connection {
 public:
  TCPConnection(std::shared_ptr<boost::asio::ip::tcp::socket> connection)
      : _connection(connection), _local_endpoint(_connection->local_endpoint()), _remote_endpoint(_connection->remote_endpoint()) {}

  virtual bool start() override { return true; }

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_read_some(buffer, handler);
  }

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_write_some(buffer, handler);
  }

  virtual boost::asio::ip::tcp::endpoint local_endpoint() override { return _local_endpoint; }
  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote_endpoint; }

  virtual bool is_open() override { return _connection->is_open(); }

  virtual void shutdown() override {
    boost::system::error_code ec;
    _connection->shutdown(ip::tcp::socket::shutdown_both, ec);
  }

  virtual void close() override { _connection->close(); }

  virtual std::string transport_name() const override { return "tcp"; }

 protected:
  std::shared_ptr<boost::asio::ip::tcp::socket> _connection;
  boost::asio::ip::tcp::endpoint _local_endpoint;
  boost::asio::ip::tcp::endpoint _remote_endpoint;
};

}  // namespace athenasip::servers
