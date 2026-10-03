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

// The HttpMiddleware function type.
// Parameters:
// - The incoming HTTP request (const reference).
// - A shared pointer to the HTTP response to be modified.
// - A "next" callback: if called with true, processing continues; if called with false,
//   the chain stops and the current response is sent.
// The request, the address it came from, the response to fill and what to call next.
// The address is the peer of the connection: there is no X-Forwarded-For, because a
// header anybody can set is not something to limit or log a caller by.
typedef std::function<void(const http::request<http::string_body>&, const std::string&, std::shared_ptr<http::response<http::string_body>>,
                           std::function<void(bool)>)>
    HttpMiddleware;

class AdminAPI {
 public:
  // Public middleware chain.
  // Middleware functions are invoked in order for each incoming request.
  std::vector<HttpMiddleware> middlewares;

  // Constructs the server using the provided logger, bind address, and port.
  AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string& bind_address, unsigned short port);
  ~AdminAPI();

  // A second listener, speaking HTTPS, beside the plain one and serving the same chain.
  // Called before start(). The plain listener stays: a healthcheck on loopback and a
  // provisioning script on the host have no use for a certificate, and a browser has no
  // use for a page that is not a secure context - it gives one no microphone. False when
  // the certificate or the port could not be had, and then there is no HTTPS listener.
  bool tls_enable(const std::string& bind_address, unsigned short port, const std::string& cert, const std::string& key);

  // Start and stop the server.
  void start();
  void stop();

  // The port actually being listened on, which is not the configured one when that was
  // zero and the operating system chose. A test binds without picking a number that
  // something else on the machine may already hold.
  std::uint16_t port() const;

  // The same for the HTTPS listener, zero when there is none.
  std::uint16_t tls_port() const;

  // The executor the API runs on. Provisioning hands this to the datastore so answers
  // come back on the API's own thread: the admin API is not on the Core strand and must
  // never put a request on the call path.
  net::any_io_executor executor() { return _io_context.get_executor(); }

  // Static helper middleware functions for common responses.
  // These functions return a middleware that sends the appropriate HTTP response and stops the chain.
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

  // Initiates an asynchronous accept operation.
  void _do_accept();
  void _do_accept_tls();

  // The sessions read the middleware chain and the logger.
  template <typename Stream>
  friend class HttpSession;
};

}  // namespace athenasip::api