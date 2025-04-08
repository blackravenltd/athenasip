#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

namespace athenasip {

class SIPCore;

namespace servers {

namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

using namespace athenasip::loggers;

class WebsocketHTTPSession : public std::enable_shared_from_this<WebsocketHTTPSession> {
 public:
  // Construct with a shared pointer to the accepted TCP socket.
  explicit WebsocketHTTPSession(std::shared_ptr<Logger> logger, std::shared_ptr<SIPCore> core, std::shared_ptr<tcp::socket> socket)
      : _logger(std::make_shared<LoggerScoped>("websocket_connection", logger)), _core(core), _socket(socket) {}

  // Start reading the HTTP request.
  void start();

 private:
  // Read the HTTP upgrade request.
  void do_read();

  // Called after the HTTP request is read.
  void on_read(boost::system::error_code ec, std::size_t bytes_transferred);

  // Perform the WebSocket handshake.
  void do_upgrade();

  std::shared_ptr<Logger> _logger;
  std::shared_ptr<tcp::socket> _socket;
  boost::beast::flat_buffer _buffer;
  http::request<http::string_body> _req;
  std::shared_ptr<SIPCore> _core;
};

}  // namespace servers
}  // namespace athenasip
