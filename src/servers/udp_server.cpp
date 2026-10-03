//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "udp_server.h"

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <boost/bind/bind.hpp>

#include "../channel.h"
#include "udp_connection.h"

using namespace boost::asio;
using namespace boost::asio::ip;

namespace athenasip::servers {

UDPServer::UDPServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port)
    : Server(std::make_unique<LoggerScoped>("udp_server", logger), core),
      _io_context(),
      _strand(boost::asio::make_strand(_io_context)),
      _socket(_io_context, udp::endpoint(ip::make_address(bind_address), port)),
      _port(port) {}

void UDPServer::start() {
  _logger->debug("Starting...");

  // Begin receiving datagrams.
  boost::asio::post(_strand, [this]() {
    _logger->info("Listening on " + _socket.local_endpoint().address().to_string() + ":" + std::to_string(_port) + " (udp://)");
    start_receive();
  });

  // Run the io_context in its own thread.
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
}

void UDPServer::stop() {
  _logger->debug("Stopping...");

  if (_thread) {
    _io_context.stop();
    if (_thread->joinable()) {
      _thread->join();
      _thread.reset();
    }
    _logger->info("Stopped");
  } else {
    _logger->debug("Already Stopped");
  }
}

void UDPServer::start_receive() {
  // Obtain a shared pointer to this instance.
  auto self = this->shared_from_this();

  // Preallocate a buffer for the incoming datagram.
  auto buffer = std::make_shared<std::vector<char>>(MAX_PACKET_SIZE);

  // Store sender endpoint
  auto sender_endpoint = std::make_shared<udp::endpoint>();

  // Start the packet receive. The completion comes back on the strand, which is the
  // only place this socket and the connection map are touched.
  _socket.async_receive_from(boost::asio::buffer(*buffer), *sender_endpoint,
                             [this, self, buffer, sender_endpoint](const boost::system::error_code& error, std::size_t bytes_transferred) {
                               boost::asio::dispatch(_strand, [this, self, buffer, sender_endpoint, error, bytes_transferred]() {
                                 this->handle_receive_from(error, bytes_transferred, buffer, sender_endpoint);
                               });
                             });
}

void UDPServer::handle_receive_from(const boost::system::error_code& error, std::size_t bytes_transferred, std::shared_ptr<std::vector<char>> buffer,
                                    std::shared_ptr<udp::endpoint> sender_endpoint) {
  auto self = std::static_pointer_cast<UDPServer>(this->shared_from_this());

  // Copy the sender endpoint from the heap.
  auto sender_endpoint_str = sender_endpoint->address().to_string() + ":" + std::to_string(sender_endpoint->port());

  if (!error) {
    buffer->resize(bytes_transferred);

    // Look up or create the UDPConnection.
    // A connection that has closed but not yet left the map is not one to deliver to: its
    // channel has gone, and nothing would read what it was given.
    std::shared_ptr<UDPConnection> connection;
    auto it = _connections.find(sender_endpoint_str);
    if (it != _connections.end() && it->second->is_open()) {
      connection = it->second;
    } else {
      connection = std::make_shared<UDPConnection>(self, _socket.local_endpoint(), *sender_endpoint);

      // As every other server does for its own connections. Without it a UDP flow answers
      // is_open() false for its whole life, and Channel::close() - which only tears a
      // connection down if it says it is open - would quietly skip it, leaving the flow in
      // this map for the process to carry.
      connection->start();

      _connections[sender_endpoint_str] = connection;
      _logger->info("New UDP Endpoint Accepted: " + sender_endpoint_str);

      auto new_channel = std::make_shared<Channel>(_logger->base_logger(), _core, connection);
      new_channel->start();
    }

    // Deliver the preallocated buffer to the connection without copying.
    connection->deliver(buffer);
  } else {
    _logger->error("UDP Receive Error: " + error.message());
  }

  // Start accepting the next packet.
  boost::asio::post(_strand, [this]() { start_receive(); });
}

void UDPServer::async_send_to(boost::asio::const_buffer buffer, boost::asio::ip::udp::endpoint remote_endpoint,
                              std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  // Called from the Core strand. The buffer stays alive because the caller's completion
  // handler owns it, and that is not run until the send finishes.
  auto self = this->shared_from_this();

  boost::asio::dispatch(_strand, [this, self, buffer, remote_endpoint, handler = std::move(handler)]() {
    _socket.async_send_to(buffer, remote_endpoint, [this, self, handler](const boost::system::error_code& ec, std::size_t size) { handler(ec, size); });
  });
}

boost::asio::ip::tcp::endpoint UDPServer::local_endpoint() {
  auto ep = _socket.local_endpoint();
  return boost::asio::ip::tcp::endpoint(ep.address(), ep.port());
}

// The connection is made here, on this server's strand, because the map is this strand's
// and a datagram from the same peer may be arriving while it is asked for. The channel is
// the caller's to make: one made here as well would be a second reader for one peer.
bool UDPServer::open_datagram_flow(boost::asio::ip::udp::endpoint remote, std::function<void(std::shared_ptr<Connection>)> handler) {
  boost::system::error_code ec;
  const auto local = _socket.local_endpoint(ec);

  // A socket bound to one family cannot send to the other.
  if (ec || local.address().is_v4() != remote.address().is_v4()) return false;

  auto self = std::static_pointer_cast<UDPServer>(this->shared_from_this());

  boost::asio::dispatch(_strand, [this, self, remote, handler = std::move(handler)]() {
    const auto key = remote.address().to_string() + ":" + std::to_string(remote.port());

    std::shared_ptr<UDPConnection> connection;

    // The registry had no channel for this peer or it would not have asked, so a live entry
    // here is one a datagram has just made and whose channel is on its way. Answering it
    // would be that second reader; the caller is told no and finds the channel instead.
    auto it = _connections.find(key);
    if (it != _connections.end() && it->second->is_open()) {
      boost::asio::post(_core->strand(), [handler]() { handler(nullptr); });
      return;
    }

    connection = std::make_shared<UDPConnection>(self, _socket.local_endpoint(), remote);
    connection->start();
    _connections[key] = connection;

    boost::asio::post(_core->strand(), [handler, connection]() { handler(connection); });
  });

  return true;
}

void UDPServer::remove_connection(const std::string& key, const Connection* connection) {
  auto self = this->shared_from_this();
  boost::asio::dispatch(_strand, [this, self, key, connection]() {
    auto it = _connections.find(key);
    if (it != _connections.end() && it->second.get() == connection) _connections.erase(it);
  });
}

}  // namespace athenasip::servers
