//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Example-only code: Asynchronous UDP RTPproxy client with Boost.Asio
// featuring robust error handling, concurrency via strands, etc.
//
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "rtp_proxy_client.h"

namespace athenasip::clients {

RTPProxyClient::RTPProxyClient(std::shared_ptr<Logger> logger, const std::string& rtpproxy_host, unsigned short rtpproxy_port)
    : _logger(std::make_unique<LoggerScoped>("rtpproxy", logger)),
      _io_context(detail::get_global_io_context())  // uses the global io_context
      ,
      _strand(boost::asio::make_strand(_io_context)),
      _socket(_strand),
      _rtpproxy_endpoint(boost::asio::ip::make_address(rtpproxy_host), rtpproxy_port) {}

bool RTPProxyClient::open() {
  boost::system::error_code ec;
  _socket.open(boost::asio::ip::udp::v4(), ec);
  if (ec) {
    _logger->error("Open Failed: " + ec.message());
    return false;
  }
  _logger->info("Opened");
  return true;
}

// Safe to call more than once.
void RTPProxyClient::close() {
  auto self = shared_from_this();

  boost::asio::dispatch(_strand, [this, self]() {
    if (_socket.is_open()) {
      boost::system::error_code ec;
      _socket.close(ec);
      if (ec) {
        _logger->error("close() failed: " + ec.message());
        return;
      }
      _logger->info("Closed");
    }
  });
}

// RTPproxy text protocol commands. The cookie is echoed back in the response; ip2 and its ports are optional.
//
// L: look up or create a session.
void RTPProxyClient::command_l(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                              const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "L " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  send_command_async(std::move(cmd), std::move(callback));
}

void RTPProxyClient::command_v(ResponseCallback callback) {
  std::string cmd = "V";
  send_command_async(std::move(cmd), std::move(callback));
}

// U: update or create a session.
void RTPProxyClient::command_u(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                              const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "U " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  send_command_async(std::move(cmd), std::move(callback));
}

// R: record a session.
void RTPProxyClient::command_r(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag, int media_idx,
                              const std::string& ip1, int rtp_port1, int rtcp_port1, const std::string& ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "R " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  send_command_async(std::move(cmd), std::move(callback));
}

// D: delete a session.
void RTPProxyClient::command_d(const std::string& cookie, const std::string& call_id, const std::string& from_tag, const std::string& to_tag,
                              ResponseCallback callback) {
  std::string cmd = "D " + cookie + " " + call_id + " " + from_tag + " " + to_tag;
  send_command_async(std::move(cmd), std::move(callback));
}

void RTPProxyClient::send_command_async(std::string command, ResponseCallback callback) {
  // Commands are serialised on the strand.
  boost::asio::post(_strand, [self = shared_from_this(), cmd = std::move(command), cb = std::move(callback)]() mutable {
    self->send_command(std::move(cmd), std::move(cb));
  });
}

// On the strand: send, then wait for the response.
void RTPProxyClient::send_command(std::string command, ResponseCallback callback) {
  if (!_socket.is_open()) {
    boost::system::error_code ec = make_error_code(boost::system::errc::bad_file_descriptor);
    if (callback) callback(nullptr, ec);
    _logger->error("Socket not open (command=" + command + ")");
    return;
  }

  _logger->info("> " + command);

  // _send_buffer must outlive the async send.
  _send_buffer = std::move(command);

  _socket.async_send_to(
      boost::asio::buffer(_send_buffer), _rtpproxy_endpoint,
      boost::asio::bind_executor(_strand, [self = shared_from_this(), callback](const boost::system::error_code& ec, std::size_t bytes_sent) mutable {
        self->handle_send(ec, bytes_sent, std::move(callback));
      }));
}

void RTPProxyClient::handle_send(const boost::system::error_code& ec, std::size_t bytes_sent, ResponseCallback callback) {
  auto self = shared_from_this();

  if (ec) {
    if (callback) callback(nullptr, ec);
    _logger->error("send_to error: " + ec.message());
    return;
  }

  _logger->info("Sent " + std::to_string(bytes_sent) + " bytes: [" + _send_buffer + "]");

  _recv_buffer.fill('\0');

  _socket.async_receive_from(boost::asio::buffer(_recv_buffer), _sender_endpoint,
                             boost::asio::bind_executor(_strand, [this, self, callback](const boost::system::error_code& ec2, std::size_t bytes_recvd) mutable {
                               handle_receive(ec2, bytes_recvd, std::move(callback));
                             }));
}

void RTPProxyClient::handle_receive(const boost::system::error_code& ec, std::size_t bytes_recvd, ResponseCallback callback) {
  if (ec) {
    if (callback) callback(nullptr, ec);
    _logger->error("receive_from error: " + ec.message());
    return;
  }

  std::string response(_recv_buffer.data(), bytes_recvd);
  _logger->info("< " + response);

  if (callback) callback(std::make_shared<RTPProxyResponse>(response), ec);
}
}  // namespace athenasip::clients
