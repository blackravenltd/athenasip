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

  UDPServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port);

  void start() override;
  void stop() override;

  // Sends for UDPConnection::async_write_some.
  void async_send_to(boost::asio::const_buffer buffer, boost::asio::ip::udp::endpoint remote_endpoint,
                     std::function<void(const boost::system::error_code&, std::size_t)> handler);

  // The UDP socket's local endpoint, as a TCP endpoint.
  boost::asio::ip::tcp::endpoint local_endpoint();

  // The executor of every connection on this server, since one socket serves them all.
  boost::asio::any_io_executor executor() { return _strand; }

  bool open_datagram_flow(boost::asio::ip::udp::endpoint remote, std::function<void(std::shared_ptr<Connection>)> handler) override;

  // Removes that connection only: by the time a closing one is removed, a new flow to the same peer may hold its key.
  void remove_connection(const std::string& key, const Connection* connection);

 protected:
  void start_receive();
  void handle_receive_from(const boost::system::error_code& error, std::size_t bytes_transferred, std::shared_ptr<std::vector<char>> buffer,
                           std::shared_ptr<boost::asio::ip::udp::endpoint> sender_endpoint);

  boost::asio::io_context _io_context;

  // One socket serves every UDP channel. Sends arrive from the Core strand while this server's thread receives, so
  // every operation on the socket goes through this strand.
  boost::asio::strand<boost::asio::io_context::executor_type> _strand;

  boost::asio::ip::udp::socket _socket;
  uint16_t _port;
  std::shared_ptr<std::thread> _thread;

  // Keyed by remote host:port.
  std::unordered_map<std::string, std::shared_ptr<UDPConnection>> _connections;
};

}  // namespace athenasip::servers
