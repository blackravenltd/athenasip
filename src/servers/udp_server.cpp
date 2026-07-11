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
      _socket(_io_context, udp::endpoint(ip::make_address(bind_address), port)),
      _port(port) {}

void UDPServer::start() {
  _logger->debug("Starting...");

  // Begin receiving datagrams.
  boost::asio::post(_io_context, [this]() {
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

  // Start the packet receive
  _socket.async_receive_from(boost::asio::buffer(*buffer), *sender_endpoint,
                             [this, self, buffer, sender_endpoint](const boost::system::error_code& error, std::size_t bytes_transferred) {
                               this->handle_receive_from(error, bytes_transferred, buffer, sender_endpoint);
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
    std::shared_ptr<UDPConnection> connection;
    auto it = _connections.find(sender_endpoint_str);
    if (it != _connections.end()) {
      connection = it->second;
    } else {
      connection = std::make_shared<UDPConnection>(self, _socket.local_endpoint(), *sender_endpoint);
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
  boost::asio::post(_io_context, [this]() { start_receive(); });
}

void UDPServer::async_send_to(boost::asio::const_buffer buffer, boost::asio::ip::udp::endpoint remote_endpoint,
                              std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  _socket.async_send_to(buffer, remote_endpoint, [this, handler](const boost::system::error_code& ec, std::size_t size) { handler(ec, size); });
}

boost::asio::ip::tcp::endpoint UDPServer::local_endpoint() {
  auto ep = _socket.local_endpoint();
  return boost::asio::ip::tcp::endpoint(ep.address(), ep.port());
}

void UDPServer::remove_connection(const std::string& key) { _connections.erase(key); }

}  // namespace athenasip::servers
