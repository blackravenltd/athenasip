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

  boost::asio::post(_strand, [this]() {
    _logger->info("Listening on " + _socket.local_endpoint().address().to_string() + ":" + std::to_string(_port) + " (udp://)");
    start_receive();
  });

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
  auto self = this->shared_from_this();

  auto buffer = std::make_shared<std::vector<char>>(MAX_PACKET_SIZE);

  auto sender_endpoint = std::make_shared<udp::endpoint>();

  // The completion runs on the strand, the only place the socket and the connection map are touched.
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

  auto sender_endpoint_str = sender_endpoint->address().to_string() + ":" + std::to_string(sender_endpoint->port());

  if (!error) {
    buffer->resize(bytes_transferred);

    // A connection that has closed but not yet left the map has no channel to read what it is given, so it is replaced.
    std::shared_ptr<UDPConnection> connection;
    auto it = _connections.find(sender_endpoint_str);
    if (it != _connections.end() && it->second->is_open()) {
      connection = it->second;
    } else {
      connection = std::make_shared<UDPConnection>(self, _socket.local_endpoint(), *sender_endpoint);

      // Without start() the flow would report is_open() false, Channel::close() would skip its teardown, and the entry
      // would stay in the map.
      connection->start();

      _connections[sender_endpoint_str] = connection;
      _logger->info("New UDP Endpoint Accepted: " + sender_endpoint_str);

      auto new_channel = std::make_shared<Channel>(_logger->base_logger(), _core, connection);
      new_channel->start();
    }

    // Delivered without copying.
    connection->deliver(buffer);
  } else {
    _logger->error("UDP Receive Error: " + error.message());
  }

  boost::asio::post(_strand, [this]() { start_receive(); });
}

void UDPServer::async_send_to(boost::asio::const_buffer buffer, boost::asio::ip::udp::endpoint remote_endpoint,
                              std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  // Called from the Core strand. The caller's completion handler owns the buffer and runs only once the send finishes.
  auto self = this->shared_from_this();

  boost::asio::dispatch(_strand, [this, self, buffer, remote_endpoint, handler = std::move(handler)]() {
    _socket.async_send_to(buffer, remote_endpoint, [this, self, handler](const boost::system::error_code& ec, std::size_t size) { handler(ec, size); });
  });
}

boost::asio::ip::tcp::endpoint UDPServer::local_endpoint() {
  auto ep = _socket.local_endpoint();
  return boost::asio::ip::tcp::endpoint(ep.address(), ep.port());
}

// The connection is made on this server's strand, which owns the map. The channel is the caller's to make: one made
// here too would be a second reader for the peer.
bool UDPServer::open_datagram_flow(boost::asio::ip::udp::endpoint remote, std::function<void(std::shared_ptr<Connection>)> handler) {
  boost::system::error_code ec;
  const auto local = _socket.local_endpoint(ec);

  // A socket bound to one family cannot send to the other.
  if (ec || local.address().is_v4() != remote.address().is_v4()) return false;

  auto self = std::static_pointer_cast<UDPServer>(this->shared_from_this());

  boost::asio::dispatch(_strand, [this, self, remote, handler = std::move(handler)]() {
    const auto key = remote.address().to_string() + ":" + std::to_string(remote.port());

    std::shared_ptr<UDPConnection> connection;

    // A live entry was just made by an arriving datagram and its channel is on its way; the caller is refused and
    // finds that channel instead.
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
