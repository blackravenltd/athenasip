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

namespace athenasip {

class TLSConnection : public Connection {
 public:
  TLSConnection(std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection) : _connection(connection) {
    auto rep = _connection->lowest_layer().remote_endpoint();
    _remote_endpoint_name = rep.address().to_string() + ":" + std::to_string(rep.port());
  }

  virtual bool start() override {
    try {
      _connection->handshake(ssl::stream_base::server);
    } catch (const std::exception& e) {
      return false;
    }
    return true;
  }

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_read_some(buffer, handler);
  }

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_write_some(buffer, handler);
  }

  virtual std::string remote_endpoint_name() override {
    return _remote_endpoint_name;
  }

  virtual bool is_open() override { return _connection->lowest_layer().is_open(); }

  virtual void shutdown() override {
    boost::system::error_code ec;
    _connection->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ec);
  }

  virtual void close() override { _connection->lowest_layer().close(); }

  virtual std::string transport_name() const override { return "tls"; }

 protected:
  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _connection;
  std::string _remote_endpoint_name;
};

}  // namespace athenasip
