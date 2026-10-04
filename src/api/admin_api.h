//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
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

// The request, the peer address, the response to fill, and next: next(true) continues the
// chain, next(false) stops it and sends the response. The address is the connection's
// peer; X-Forwarded-For is not read, because anybody can set it.
typedef std::function<void(const http::request<http::string_body>&, const std::string&, std::shared_ptr<http::response<http::string_body>>,
                           std::function<void(bool)>)>
    HttpMiddleware;

class AdminAPI {
 public:
  // Invoked in order for each request.
  std::vector<HttpMiddleware> middlewares;

  AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string& bind_address, unsigned short port);
  ~AdminAPI();

  // Adds an HTTPS listener beside the plain one, serving the same chain. Call before
  // start(). Returns false, and adds no listener, when the certificate or port cannot be had.
  bool tls_enable(const std::string& bind_address, unsigned short port, const std::string& cert, const std::string& key);

  void start();
  void stop();

  // The port being listened on, which differs from the configured one when that was zero.
  std::uint16_t port() const;

  // The HTTPS listener's port, zero when there is none.
  std::uint16_t tls_port() const;

  // The executor the API runs on. It is not the Core strand; datastore answers for API
  // requests come back here, off the call path.
  net::any_io_executor executor() { return _io_context.get_executor(); }

  // Middleware that sends a fixed response and stops the chain.
  static HttpMiddleware send_status_end(uint16_t code, std::string message);

  static HttpMiddleware send_200_end();
  static HttpMiddleware send_400_end();
  static HttpMiddleware send_404_end();
  static HttpMiddleware send_500_end();

 private:
  std::shared_ptr<athenasip::loggers::Logger> _logger;
  std::string _bind_address;
  unsigned short _port;
  net::io_context _io_context;
  tcp::acceptor _acceptor;
  std::shared_ptr<std::thread> _thread;

  std::shared_ptr<net::ssl::context> _tls_context;
  tcp::acceptor _tls_acceptor;
  std::string _tls_bind_address;

  static bool _listen(tcp::acceptor& acceptor, const std::string& bind_address, unsigned short port, athenasip::loggers::Logger& logger);

  void _do_accept();
  void _do_accept_tls();

  // Sessions read the middleware chain and the logger.
  template <typename Stream>
  friend class HttpSession;
};

}  // namespace athenasip::api