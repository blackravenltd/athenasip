//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Example-only code: Asynchronous UDP RTPproxy client with Boost.Asio
// featuring robust error handling, concurrency via strands, etc.
//
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "rtp_proxy_response.h"

using namespace athenasip::loggers;

namespace athenasip::clients {

class RTPProxyClient : public std::enable_shared_from_this<RTPProxyClient> {
 public:
  using ResponseCallback = std::function<void(std::shared_ptr<RTPProxyResponse> response, const boost::system::error_code& ec)>;

  RTPProxyClient(std::shared_ptr<Logger> logger, const std::string& rtpproxy_host, unsigned short rtpproxy_port);

  bool open();
  void close();

  void command_l(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2, ResponseCallback callback);

  void command_u(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2, ResponseCallback callback);

  void command_r(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2, ResponseCallback callback);

  void command_d(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, ResponseCallback callback);

  void command_v(ResponseCallback callback);

 private:
  std::shared_ptr<Logger> _logger;
  boost::asio::io_context& _io_context;
  boost::asio::strand<boost::asio::io_context::executor_type> _strand;

  boost::asio::ip::udp::socket _socket;
  boost::asio::ip::udp::endpoint _rtpproxy_endpoint;

  // Buffers must outlive the async ops
  std::string _send_buffer;
  std::array<char, 1024> _recv_buffer{};

  boost::asio::ip::udp::endpoint _sender_endpoint;

  void send_command_async(std::string command, ResponseCallback callback);
  void send_command(std::string command, ResponseCallback callback);
  void handle_send(const boost::system::error_code& ec, std::size_t bytes_sent, ResponseCallback callback);
  void handle_receive(const boost::system::error_code& ec, std::size_t bytes_recvd, ResponseCallback callback);
};

}  // namespace athenasip::clients
