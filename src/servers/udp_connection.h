//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../async_queue.h"
#include "connection.h"

namespace athenasip::servers {

class UDPServer;

class UDPConnection : public Connection {
 public:
  UDPConnection(std::weak_ptr<UDPServer> udp_server, boost::asio::ip::udp::endpoint local_endpoint, boost::asio::ip::udp::endpoint remote_endpoint);

  virtual bool start() override;

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override;

  virtual boost::asio::ip::tcp::endpoint local_endpoint() override;
  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override;
  virtual std::string remote_endpoint_name() override;

  virtual bool is_open() override;

  virtual bool is_reliable() override { return false; }

  virtual void shutdown() override;
  virtual void close() override;

  void deliver(std::shared_ptr<std::vector<char>> buffer);

  virtual std::string transport_name() const override;

 protected:
  std::weak_ptr<UDPServer> _udp_server;
  boost::asio::ip::udp::endpoint _local_endpoint;
  boost::asio::ip::udp::endpoint _remote_endpoint;

  AsyncQueue<std::shared_ptr<std::vector<char>>> _buffer_queue;

  bool _is_open;
};

}  // namespace athenasip::servers
