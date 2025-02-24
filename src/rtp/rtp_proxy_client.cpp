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

RTPProxyClient::RTPProxyClient(std::shared_ptr<Logger> logger, const std::string &rtpproxy_host, unsigned short rtpproxy_port)
    : _logger(std::make_unique<LoggerScoped>("rtpproxy", logger)),
      _io_context(detail::getGlobalIOContext())  // uses the global io_context
      ,
      _strand(boost::asio::make_strand(_io_context)),
      _socket(_strand),
      _rtpproxy_endpoint(boost::asio::ip::make_address(rtpproxy_host), rtpproxy_port) {}

/**
 * @brief Opens (binds) the UDP socket so we can send and receive commands.
 * @return true on success, false otherwise (logs error).
 */
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

/**
 * @brief Closes the underlying socket. Safe to call multiple times.
 */
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

// --------------------------------------------------------------------------
//  Public Methods - RTPproxy Text Protocol commands: L, U, R, D
// --------------------------------------------------------------------------
/**
 * @brief Sends an 'L' (Lookup/Create) command asynchronously.
 *
 * @param cookie       Arbitrary string (echoed back by RTPproxy).
 * @param call_id      SIP Call-ID
 * @param from_tag     SIP From-tag
 * @param to_tag       SIP To-tag
 * @param media_idx    Media index, typically 1
 * @param ip1          IP address (caller)
 * @param rtp_port1    RTP port (caller)
 * @param rtcp_port1   RTCP port (caller)
 * @param ip2          Optional second IP (callee)
 * @param rtp_port2    RTP port (callee)
 * @param rtcp_port2   RTCP port (callee)
 * @param callback     Called when response arrives or on error.
 */
void RTPProxyClient::commandL(const std::string &cookie, const std::string &call_id, const std::string &from_tag, const std::string &to_tag, int media_idx,
                              const std::string &ip1, int rtp_port1, int rtcp_port1, const std::string &ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "L " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  asyncSendCommand(std::move(cmd), std::move(callback));
}

void RTPProxyClient::commandV(ResponseCallback callback) {
  std::string cmd = "V";
  asyncSendCommand(std::move(cmd), std::move(callback));
}

/**
 * @brief Sends a 'U' (Update/Create) command asynchronously.
 */
void RTPProxyClient::commandU(const std::string &cookie, const std::string &call_id, const std::string &from_tag, const std::string &to_tag, int media_idx,
                              const std::string &ip1, int rtp_port1, int rtcp_port1, const std::string &ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "U " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  asyncSendCommand(std::move(cmd), std::move(callback));
}

/**
 * @brief Sends an 'R' (Record) command asynchronously.
 */
void RTPProxyClient::commandR(const std::string &cookie, const std::string &call_id, const std::string &from_tag, const std::string &to_tag, int media_idx,
                              const std::string &ip1, int rtp_port1, int rtcp_port1, const std::string &ip2, int rtp_port2, int rtcp_port2,
                              ResponseCallback callback) {
  std::string cmd = "R " + cookie + " " + call_id + " " + from_tag + " " + to_tag + " " + std::to_string(media_idx) + " " + ip1 + " " +
                    std::to_string(rtp_port1) + " " + std::to_string(rtcp_port1);

  if (!ip2.empty()) {
    cmd += " " + ip2 + " " + std::to_string(rtp_port2) + " " + std::to_string(rtcp_port2);
  }

  asyncSendCommand(std::move(cmd), std::move(callback));
}

/**
 * @brief Sends a 'D' (Delete) command asynchronously.
 */
void RTPProxyClient::commandD(const std::string &cookie, const std::string &call_id, const std::string &from_tag, const std::string &to_tag,
                              ResponseCallback callback) {
  std::string cmd = "D " + cookie + " " + call_id + " " + from_tag + " " + to_tag;
  asyncSendCommand(std::move(cmd), std::move(callback));
}

void RTPProxyClient::asyncSendCommand(std::string command, ResponseCallback callback) {
  // Post onto the strand so that all commands are serialized
  boost::asio::post(_strand, [self = shared_from_this(), cmd = std::move(command), cb = std::move(callback)]() mutable {
    self->doSendCommand(std::move(cmd), std::move(cb));
  });
}

/**
 * @brief Actually do the send->receive sequence on the strand.
 */
void RTPProxyClient::doSendCommand(std::string command, ResponseCallback callback) {
  if (!_socket.is_open()) {
    // Socket is closed or not opened. Return an error.
    boost::system::error_code ec = make_error_code(boost::system::errc::bad_file_descriptor);
    if (callback) callback(nullptr, ec);
    _logger->error("Socket not open (command=" + command + ")");
    return;
  }

  _logger->info("> "+command);

  // Store the command in _send_buffer so it lives throughout the async calls
  _send_buffer = std::move(command);

  // Async send
  _socket.async_send_to(
      boost::asio::buffer(_send_buffer), _rtpproxy_endpoint,
      boost::asio::bind_executor(_strand, [self = shared_from_this(), callback](const boost::system::error_code &ec, std::size_t bytes_sent) mutable {
        self->handleSend(ec, bytes_sent, std::move(callback));
      }));
}

void RTPProxyClient::handleSend(const boost::system::error_code &ec, std::size_t bytes_sent, ResponseCallback callback) {
  auto self = shared_from_this();

  if (ec) {
    // Notify callback of error
    if (callback) callback(nullptr, ec);
    _logger->error("send_to error: " + ec.message());
    return;
  }

  _logger->info("Sent " + std::to_string(bytes_sent) + " bytes: [" + _send_buffer + "]");

  // Prepare to receive a response
  _recv_buffer.fill('\0');

  // Async receive
  _socket.async_receive_from(
      boost::asio::buffer(_recv_buffer), _sender_endpoint,
      boost::asio::bind_executor(_strand, [this, self, callback](const boost::system::error_code &ec2, std::size_t bytes_recvd) mutable {
        handleReceive(ec2, bytes_recvd, std::move(callback));
      }));
}

/**
 * @brief Handle completion of async_receive_from
 */
void RTPProxyClient::handleReceive(const boost::system::error_code &ec, std::size_t bytes_recvd, ResponseCallback callback) {
  if (ec) {
    // Notify callback of error
    if (callback) callback(nullptr, ec);
    _logger->error("receive_from error: " + ec.message());
    return;
  }

  std::string response(_recv_buffer.data(), bytes_recvd);
  _logger->info("< "+response);

  // Notify caller of success
  if (callback) callback(std::make_shared<RTPProxyResponse>(response), ec);
}
}  // namespace athenasip
