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

class TLSConnection : public Connection {
 public:
  TLSConnection(std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection) : _connection(connection) {}

  virtual bool start() override {
    try {
      _connection->handshake(ssl::stream_base::server);
    } catch (const std::exception& e) {
      return false;
    }
    return true;
  }

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_read_some(buffer, [this, handler](boost::system::error_code ec, std::size_t length) {
      if (ec == boost::asio::ssl::error::stream_truncated) {
        ec = boost::asio::error::operation_aborted;
      }
      handler(ec, length);
    });
  }

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _connection->async_write_some(buffer, [this, handler](boost::system::error_code ec, std::size_t length) { handler(ec, length); });
  }

  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override { return _connection->lowest_layer().remote_endpoint(); }
  virtual boost::asio::ip::tcp::endpoint local_endpoint() override { return _connection->lowest_layer().local_endpoint(); }

  virtual bool is_open() override { return _connection->lowest_layer().is_open(); }

  virtual void shutdown() override {
    boost::system::error_code ec;
    _connection->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ec);
  }

  virtual void close() override { _connection->lowest_layer().close(); }

  virtual std::string transport_name() const override { return "tls"; }

 protected:
  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _connection;
};

}  // namespace athenasip::servers
