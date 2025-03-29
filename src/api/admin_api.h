//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

namespace athenasip::api {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// The HttpMiddleware function type.
// Parameters:
// - The incoming HTTP request (const reference).
// - A shared pointer to the HTTP response to be modified.
// - A "next" callback: if called with true, processing continues; if called with false,
//   the chain stops and the current response is sent.
typedef std::function<void(const http::request<http::string_body> &, std::shared_ptr<http::response<http::string_body>>, std::function<void(bool)>)>
    HttpMiddleware;

class AdminAPI {
 public:
  // Public middleware chain.
  // Middleware functions are invoked in order for each incoming request.
  std::vector<HttpMiddleware> middlewares;

  // Constructs the server using the provided logger, bind address, and port.
  AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string &bind_address, unsigned short port);
  ~AdminAPI();

  // Start and stop the server.
  void start();
  void stop();

  // Static helper middleware functions for common responses.
  // These functions return a middleware that sends the appropriate HTTP response and stops the chain.
  static HttpMiddleware sendStatusEnd(uint16_t code, std::string message);

  static HttpMiddleware send200end();
  static HttpMiddleware send400end();
  static HttpMiddleware send404end();
  static HttpMiddleware send500end();

 private:
  std::shared_ptr<athenasip::loggers::Logger> _logger;
  std::string _bind_address;
  unsigned short _port;
  net::io_context _io_context;
  tcp::acceptor _acceptor;
  std::shared_ptr<std::thread> _thread;

  // Initiates an asynchronous accept operation.
  void _do_accept();

  // Allow HttpSession to access the server's logger
  friend class HttpSession;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  // HttpSession now receives a reference to the server so it can access the middleware chain.
  HttpSession(tcp::socket socket, AdminAPI &server);
  void start();

 private:
  std::shared_ptr<athenasip::loggers::Logger> _logger;

  tcp::socket _socket;
  beast::flat_buffer _buffer;
  AdminAPI &_server;
  http::request<http::string_body> _req;

  // Reads the incoming HTTP request.
  void do_read();

  // Recursively process the middleware chain.
  // If next(false) is called at any middleware, the chain stops and the response is sent.
  void processMiddlewareChain(std::size_t index, std::shared_ptr<http::response<http::string_body>> res);

  // Write the final response to the client.
  void do_write(std::shared_ptr<http::response<http::string_body>> res);
};

}  // namespace athenasip::api