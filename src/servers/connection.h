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

namespace athenasip::servers {

class Connection {
 public:
  virtual ~Connection() = default;

  virtual bool start() = 0;

  // The executor this connection's stream belongs to: its server's io_context, or the
  // strand that server shares between its connections. A socket is not safe for two
  // threads at once and the server's own thread is already inside it, so everything that
  // touches the stream - starting a read, starting a write, tearing it down - runs here.
  // The Core strand hands the work over rather than doing it. UDPServer already worked
  // this way; the others reached into the socket from the strand, and tsan sees it on a
  // WebSocket whose peer is closing while the node closes its end.
  virtual boost::asio::any_io_executor executor() = 0;

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;

  virtual boost::asio::ip::tcp::endpoint remote_endpoint() = 0;
  virtual boost::asio::ip::tcp::endpoint local_endpoint() = 0;

  virtual std::string remote_endpoint_name() {
    auto rep = remote_endpoint();
    return rep.address().to_string() + ":" + std::to_string(rep.port());
  }

  virtual std::string local_endpoint_name() {
    auto rep = local_endpoint();
    return rep.address().to_string() + ":" + std::to_string(rep.port());
  }

  virtual bool is_open() = 0;

  virtual bool is_reliable() = 0;

  virtual void shutdown() = 0;

  virtual void close() = 0;

  virtual std::string transport_name() const = 0;

  // Who the far end proved it is, for a connection that made it prove anything: the
  // common name of a peer certificate the handshake verified. Empty everywhere else.
  virtual std::string peer_identity() const { return ""; }
};

}  // namespace athenasip::servers
