//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <cstddef>
#include <functional>
#include <string>

#include "servers/connection.h"

// A Connection that talks to nothing. Writes are captured so a test can assert on what
// went out, and reads never complete unless the test feeds them.
class MockConnection : public athenasip::servers::Connection {
 public:
  MockConnection(std::string transport = "tcp", std::string remote_address = "192.0.2.10", std::uint16_t remote_port = 5060,
                 std::string local_address = "192.0.2.1", std::uint16_t local_port = 5060)
      : _transport(std::move(transport)),
        _remote(boost::asio::ip::make_address(remote_address), remote_port),
        _local(boost::asio::ip::make_address(local_address), local_port) {}

  bool start() override {
    open = true;
    return true;
  }

  void async_read_some(boost::asio::mutable_buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    // Held, not called: the channel's read loop parks here until a test drives it.
    pending_read = std::move(handler);
  }

  void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    written.append(static_cast<const char*>(buffer.data()), boost::asio::buffer_size(buffer));
    write_calls++;

    if (handler) handler(write_error, boost::asio::buffer_size(buffer));
  }

  boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote; }
  boost::asio::ip::tcp::endpoint local_endpoint() override { return _local; }

  bool is_open() override { return open; }
  bool is_reliable() override { return reliable; }

  void shutdown() override { shutdown_calls++; }

  void close() override {
    open = false;
    close_calls++;
  }

  std::string transport_name() const override { return _transport; }

  // Observable state
  bool open = true;
  bool reliable = true;
  std::string written;
  int write_calls = 0;
  int close_calls = 0;
  int shutdown_calls = 0;
  boost::system::error_code write_error;
  std::function<void(const boost::system::error_code&, std::size_t)> pending_read;

 private:
  std::string _transport;
  boost::asio::ip::tcp::endpoint _remote;
  boost::asio::ip::tcp::endpoint _local;
};
