#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "server.h"

namespace athenasip::servers {

class UDPConnection;

class UDPServer : public Server {
 public:
  const uint16_t MAX_PACKET_SIZE = 65535;

  UDPServer(std::shared_ptr<Logger> logger, std::shared_ptr<SIPCore> core, const std::string &bind_address, short port);

  void start() override;
  void stop() override;

  // Called by UDPConnection::async_write_some to send data.
  void async_send_to(boost::asio::const_buffer buffer, boost::asio::ip::udp::endpoint remote_endpoint,
                     std::function<void(const boost::system::error_code &, std::size_t)> handler);

  // Returns the local endpoint of the UDP socket as a TCP endpoint.
  boost::asio::ip::tcp::endpoint local_endpoint();

  // Remove a connection from the mapping (e.g. after shutdown).
  void remove_connection(const std::string &key);

 protected:
  // Start an asynchronous receive.
  void start_receive();
  // Handle an incoming datagram.
  void handle_receive_from(const boost::system::error_code &error, std::size_t bytes_transferred, std::shared_ptr<std::vector<char>> buffer,
                           std::shared_ptr<boost::asio::ip::udp::endpoint> sender_endpoint);

  boost::asio::io_context _io_context;
  boost::asio::ip::udp::socket _socket;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;
  std::string _nonce_secret;

  // Mapping from remote endpoint key to UDPConnection.
  std::unordered_map<std::string, std::shared_ptr<UDPConnection>> _connections;
};

}  // namespace athenasip::servers
