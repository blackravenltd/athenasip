//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
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
  TCPConnection(std::shared_ptr<boost::asio::ip::tcp::socket> socket) : _socket(socket) {
    // The error_code forms: a peer that reset between the accept and this has no address,
    // and the throwing forms took the whole node down from inside the listener's handler.
    boost::system::error_code gone;
    _local_endpoint = _socket->local_endpoint(gone);
    _remote_endpoint = _socket->remote_endpoint(gone);
  }

  virtual bool start() override { return true; }

  boost::asio::any_io_executor executor() override { return _socket->get_executor(); }

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _socket->async_read_some(buffer, handler);
  }

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _socket->async_write_some(buffer, handler);
  }

  virtual boost::asio::ip::tcp::endpoint local_endpoint() override { return _local_endpoint; }
  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote_endpoint; }

  virtual bool is_open() override { return _socket->is_open(); }

  virtual bool is_reliable() override { return true; }

  virtual void shutdown() override {
    boost::system::error_code ec;
    _socket->shutdown(ip::tcp::socket::shutdown_both, ec);
  }

  virtual void close() override { _socket->close(); }

  virtual std::string transport_name() const override { return "tcp"; }

 protected:
  std::shared_ptr<boost::asio::ip::tcp::socket> _socket;
  boost::asio::ip::tcp::endpoint _local_endpoint;
  boost::asio::ip::tcp::endpoint _remote_endpoint;
};

}  // namespace athenasip::servers
