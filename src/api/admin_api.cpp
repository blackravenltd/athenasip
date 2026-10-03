//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "admin_api.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <memory>
#include <thread>

#include "../loggers/logger_scoped.h"

using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

namespace athenasip::api {

// ----------------------
// AdminAPI Implementation
// ----------------------

// Constructor: sets up the listening endpoint and prepares the acceptor.
AdminAPI::AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string& bind_address, unsigned short port)
    : _logger(std::make_shared<loggers::LoggerScoped>("admin_api", logger)), _bind_address(bind_address), _port(port), _io_context(), _acceptor(_io_context) {
  boost::system::error_code ec;
  tcp::endpoint endpoint(net::ip::make_address(_bind_address, ec), _port);
  if (ec) {
    _logger->error("Failed to create endpoint: " + ec.message());
    return;
  }
  _acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    _logger->error("Failed to open acceptor: " + ec.message());
    return;
  }
  _acceptor.set_option(net::socket_base::reuse_address(true), ec);
  if (ec) {
    _logger->error("Failed to set reuse address: " + ec.message());
    return;
  }
  _acceptor.bind(endpoint, ec);
  if (ec) {
    _logger->error("Failed to bind acceptor: " + ec.message());
    return;
  }
  _acceptor.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    _logger->error("Failed to listen on acceptor: " + ec.message());
    return;
  }
}

// Destructor: stops the server.
AdminAPI::~AdminAPI() { stop(); }

std::uint16_t AdminAPI::port() const {
  boost::system::error_code ec;
  const auto endpoint = _acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

// Start the server: begin accepting connections and run the io_context in a new thread.
void AdminAPI::start() {
  _logger->debug("Starting AdminAPI server...");
  _do_accept();
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
  _logger->info("Listening on " + _bind_address + ":" + std::to_string(_port));
}

// Stop the server and join the thread.
void AdminAPI::stop() {
  _logger->debug("Stopping AdminAPI server...");
  _io_context.stop();
  if (_thread && _thread->joinable()) {
    _thread->join();
  }
  _thread.reset();
  _logger->info("AdminAPI server stopped.");
}

// Begin accepting new connections asynchronously.
void AdminAPI::_do_accept() {
  _acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (!ec) {
      // Create a new HttpSession to handle the incoming connection.
      std::make_shared<HttpSession>(std::move(socket), *this)->start();
    } else {
      _logger->error("Accept error: " + ec.message());
    }
    // Continue accepting new connections.
    _do_accept();
  });
}

// ----------------------------------
// Static Helper Middleware Functions
// ----------------------------------

// send_status_end: set HTTP status and message and end
HttpMiddleware AdminAPI::send_status_end(uint16_t code, std::string message) {
  return [code, message](const http::request<http::string_body>& req, const std::string&, std::shared_ptr<http::response<http::string_body>> res,
                         std::function<void(bool)> next) {
    res->result(code);
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    boost::json::object obj;
    obj["message"] = message;
    res->body() = boost::json::serialize(obj);
    next(false);
  };
}
HttpMiddleware AdminAPI::send_200_end() { return AdminAPI::send_status_end(200, "OK"); }
HttpMiddleware AdminAPI::send_400_end() { return AdminAPI::send_status_end(400, "Bad Request"); }
HttpMiddleware AdminAPI::send_404_end() { return AdminAPI::send_status_end(404, "Not Found"); }
HttpMiddleware AdminAPI::send_500_end() { return AdminAPI::send_status_end(500, "Internal Server Error"); }

// --------------------------
// HttpSession Implementation
// --------------------------

// HttpSession constructor: stores the socket and a reference to the server.
HttpSession::HttpSession(tcp::socket socket, AdminAPI& server) : _socket(std::move(socket)), _server(server) {
  boost::system::error_code ec;
  auto rm = _socket.remote_endpoint(ec);
  _remote = ec ? std::string() : rm.address().to_string();
  _logger = std::make_shared<loggers::LoggerScoped>(ec ? std::string("unknown") : _remote + ":" + std::to_string(rm.port()), server._logger);
}

// Start the session by initiating an asynchronous read.
void HttpSession::start() { do_read(); }

// Asynchronously read an HTTP request.
void HttpSession::do_read() {
  auto self = shared_from_this();
  http::async_read(_socket, _buffer, _req, [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
    (void)bytes_transferred;  // Unused
    if (!ec) {
      // Create a default HTTP response.
      _logger->debug(std::string(http::to_string(_req.method())) + " " + std::string(_req.target()));
      auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, _req.version());
      // Begin processing the middleware chain from index 0.
      process_middleware_chain(0, res);
    } else {
      _server._logger->error("Error reading request: " + ec.message());
    }
  });
}

// Recursively process the middleware chain.
// If a middleware calls next(false), the chain stops and the response is sent.
void HttpSession::process_middleware_chain(std::size_t index, std::shared_ptr<http::response<http::string_body>> res) {
  if (index < _server.middlewares.size()) {
    // self, because a middleware may answer asynchronously - the provisioning routes go
    // to the datastore and come back when it does - and the session has to outlive the
    // wait. Without it the chain is only safe for middleware that answers inline.
    auto self = shared_from_this();

    auto next = [this, self, index, res](bool continueChain) {
      if (continueChain) {
        process_middleware_chain(index + 1, res);
      } else {
        res->prepare_payload();
        // Middleware has halted further processing; send the response.
        do_write(res);
      }
    };
    // Invoke the middleware at the current index.
    _server.middlewares[index](_req, _remote, res, next);
  } else {
    // All middleware have been processed; send the response.
    res->prepare_payload();
    do_write(res);
  }
}

// Asynchronously write the response and shutdown the socket.
void HttpSession::do_write(std::shared_ptr<http::response<http::string_body>> res) {
  auto self = shared_from_this();
  http::async_write(_socket, *res, [this, self, res](boost::system::error_code ec, std::size_t) {
    _socket.shutdown(tcp::socket::shutdown_send, ec);
    if (ec) {
      _server._logger->error("Error writing response: " + ec.message());
    }
  });
}

}  // namespace athenasip::api
