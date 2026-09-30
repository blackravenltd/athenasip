//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "udp_connection.h"

#include <algorithm>  // For std::min
#include <cstring>    // For std::memcpy

#include "../global_io_context.h"
#include "udp_server.h"

namespace athenasip::servers {

UDPConnection::UDPConnection(std::weak_ptr<UDPServer> udp_server, boost::asio::ip::udp::endpoint local_endpoint, boost::asio::ip::udp::endpoint remote_endpoint)
    : _udp_server(udp_server), _local_endpoint(local_endpoint), _remote_endpoint(remote_endpoint), _is_open(false) {
  auto server = _udp_server.lock();
  _executor = server ? server->executor() : boost::asio::any_io_executor(detail::get_global_io_context().get_executor());
}

bool UDPConnection::start() {
  _is_open = true;
  return _is_open;
}

void UDPConnection::async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  _buffer_queue.on_item_once([this, handler, buffer](std::shared_ptr<std::vector<char>> qb) {
    auto bytes_to_copy = std::min(boost::asio::buffer_size(buffer), qb->size());
    std::memcpy(buffer.data(), qb->data(), bytes_to_copy);
    handler(boost::system::error_code(), bytes_to_copy);
  });
}

void UDPConnection::async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) {
  if (auto server = _udp_server.lock()) {
    server->async_send_to(buffer, _remote_endpoint, handler);
  } else {
    handler(boost::system::error_code(boost::asio::error::operation_aborted), 0);
  }
}

boost::asio::ip::tcp::endpoint UDPConnection::local_endpoint() { return boost::asio::ip::tcp::endpoint(_local_endpoint.address(), _local_endpoint.port()); }

boost::asio::ip::tcp::endpoint UDPConnection::remote_endpoint() { return boost::asio::ip::tcp::endpoint(_remote_endpoint.address(), _remote_endpoint.port()); }

std::string UDPConnection::remote_endpoint_name() { return _remote_endpoint.address().to_string() + ":" + std::to_string(_remote_endpoint.port()); }

bool UDPConnection::is_open() { return _is_open; }

void UDPConnection::shutdown() { _is_open = false; }

// Everything a UDP flow has to let go of, because nothing else will.
//
// A TCP or TLS connection closing is a socket closing, which errors the pending read and
// unwinds the channel. UDP has neither: the read waits on a queue that will never be fed
// again, and the server's map holds this connection under a key nothing removes. So this
// does both by hand - releases the reader, which is holding the channel, and asks the
// server to forget the key.
void UDPConnection::close() {
  _is_open = false;

  _buffer_queue.close();

  // Keyed the way UDPServer keys it - bare host:port - and not the way Core keys a flow,
  // which prefixes the transport. Getting that wrong leaves the entry in place, and then
  // the next datagram from the address is handed to this closed connection instead of
  // making a live one: the flow is forgotten by the node and still occupies the map.
  if (auto server = _udp_server.lock()) {
    server->remove_connection(remote_endpoint_name());
  }
}

void UDPConnection::deliver(std::shared_ptr<std::vector<char>> buffer) { _buffer_queue.push(buffer); }

std::string UDPConnection::transport_name() const { return "udp"; }

}  // namespace athenasip::servers
