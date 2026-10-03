//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/asio.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "global_io_context.h"
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

  // A real connection belongs to its server's thread. By default this one belongs to the
  // global io_context, which is where the Core strand runs too, so a hand-over from the
  // strand runs inline and a test sees what it did without having to wait for it.
  //
  // own_strand() gives it an executor of its own, which is how a test tells a hand-over
  // from a reach-in: work dispatched to a strand this thread is not in has to wait, and
  // running_in_this_thread() is false for anything that arrived any other way.
  boost::asio::any_io_executor executor() override {
    if (_strand) return *_strand;
    return athenasip::detail::get_global_io_context().get_executor();
  }

  void own_strand() { _strand.emplace(boost::asio::make_strand(athenasip::detail::get_global_io_context())); }

  // Whether this call arrived on the connection's own executor. Always true when it has
  // no executor of its own, because then there is nothing to arrive on.
  bool on_own_executor() const { return !_strand || _strand->running_in_this_thread(); }

  void async_read_some(boost::asio::mutable_buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    read_on_own_executor = on_own_executor();

    // Held, not called: the channel's read loop parks here until a test drives it.
    pending_read = std::move(handler);
  }

  void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    write_on_own_executor = on_own_executor();

    // A real socket may take less than it was given. write_limit is how a test says so.
    const auto offered = boost::asio::buffer_size(buffer);
    const auto taken = write_limit > 0 ? std::min(write_limit, offered) : offered;

    written.append(static_cast<const char*>(buffer.data()), taken);
    write_calls++;

    // Held rather than answered, so a test can see what the channel does while a write
    // is still in flight.
    if (defer_writes) {
      pending_write = std::move(handler);
      pending_write_bytes = taken;
      return;
    }

    if (handler) handler(write_error, taken);
  }

  // Answer the write that was held. Returns false when there was none.
  bool complete_write() {
    auto handler = std::move(pending_write);
    pending_write = nullptr;
    if (!handler) return false;

    handler(write_error, pending_write_bytes);
    return true;
  }

  boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote; }
  boost::asio::ip::tcp::endpoint local_endpoint() override { return _local; }

  bool is_open() override {
    is_open_on_own_executor = on_own_executor();
    return open;
  }
  bool is_reliable() override { return reliable; }

  void shutdown() override {
    shutdown_on_own_executor = on_own_executor();
    shutdown_calls++;
  }

  void close() override {
    close_on_own_executor = on_own_executor();
    open = false;
    close_calls++;
  }

  std::string transport_name() const override { return _transport; }

  // The node a cluster-CA certificate named, as the inter-node listener reports it. Empty
  // for everything that is not a peer.
  std::string peer_identity() const override { return peer; }
  std::string peer;

  // Observable state
  bool open = true;
  bool reliable = true;
  std::string written;
  int write_calls = 0;
  int close_calls = 0;
  int shutdown_calls = 0;
  boost::system::error_code write_error;
  std::function<void(const boost::system::error_code&, std::size_t)> pending_read;

  // Write behaviour a test can steer: hold the completion, and take only so many bytes.
  bool defer_writes = false;
  std::size_t write_limit = 0;
  std::function<void(const boost::system::error_code&, std::size_t)> pending_write;
  std::size_t pending_write_bytes = 0;

  // Where each operation was called from. Nothing may touch the stream from anywhere but
  // the connection's own executor.
  bool read_on_own_executor = false;
  bool write_on_own_executor = false;
  bool is_open_on_own_executor = false;
  bool shutdown_on_own_executor = false;
  bool close_on_own_executor = false;

 private:
  std::optional<boost::asio::strand<boost::asio::io_context::executor_type>> _strand;
  std::string _transport;
  boost::asio::ip::tcp::endpoint _remote;
  boost::asio::ip::tcp::endpoint _local;
};
