//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
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
AdminAPI::AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger,
                   const std::string &bind_address,
                   unsigned short port)
    : _logger(std::make_shared<loggers::LoggerScoped>("admin_api",logger)),
      _bind_address(bind_address),
      _port(port),
      _io_context(),
      _acceptor(_io_context) {
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

// ------------------------------
// Static Helper Middleware Functions
// ------------------------------

// send200end: prepares a 200 OK JSON response and stops further middleware processing.
HttpMiddleware AdminAPI::send200end() {
  return [](const http::request<http::string_body>& req,
            std::shared_ptr<http::response<http::string_body>> res,
            std::function<void(bool)> next) {
    res->result(http::status::ok);
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    boost::json::object obj;
    obj["message"] = "OK";
    res->body() = boost::json::serialize(obj);
    next(false); // Stop processing the middleware chain.
  };
}

// send400end: prepares a 400 Bad Request JSON response and stops further middleware processing.
HttpMiddleware AdminAPI::send400end() {
  return [](const http::request<http::string_body>& req,
            std::shared_ptr<http::response<http::string_body>> res,
            std::function<void(bool)> next) {
    res->result(http::status::bad_request);
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    boost::json::object obj;
    obj["code"] = 400;
    obj["message"] = "Bad Request";
    res->body() = boost::json::serialize(obj);
    next(false);
  };
}

HttpMiddleware AdminAPI::send404end() {
  return [](const http::request<http::string_body>& req,
            std::shared_ptr<http::response<http::string_body>> res,
            std::function<void(bool)> next) {
    res->result(http::status::not_found);
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    boost::json::object obj;
    obj["code"] = 404;
    obj["message"] = "Not Found";
    res->body() = boost::json::serialize(obj);
    next(false);
  };
}

// send500end: prepares a 500 Internal Server Error JSON response and stops further middleware processing.
HttpMiddleware AdminAPI::send500end() {
  return [](const http::request<http::string_body>& req,
            std::shared_ptr<http::response<http::string_body>> res,
            std::function<void(bool)> next) {
    res->result(http::status::internal_server_error);
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    boost::json::object obj;
    obj["code"] = 500;
    obj["message"] = "Internal Server Error";
    res->body() = boost::json::serialize(obj);
    next(false);
  };
}

// ----------------------
// HttpSession Implementation
// ----------------------

// HttpSession constructor: stores the socket and a reference to the server.
HttpSession::HttpSession(tcp::socket socket, AdminAPI &server)
    : _socket(std::move(socket)), _server(server) {
      auto rm = _socket.remote_endpoint();
      _logger = std::make_shared<loggers::LoggerScoped>(rm.address().to_string()+":"+std::to_string(rm.port()), server._logger);
}

// Start the session by initiating an asynchronous read.
void HttpSession::start() { 
  do_read();
}

// Asynchronously read an HTTP request.
void HttpSession::do_read() {
  auto self = shared_from_this();
  http::async_read(_socket, _buffer, _req,
                   [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
                     (void)bytes_transferred; // Unused
                     if (!ec) {
                       // Create a default HTTP response.
                      _logger->debug(std::string(http::to_string(_req.method()))+" "+std::string(_req.target()));
                       auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, _req.version());
                       // Begin processing the middleware chain from index 0.
                       processMiddlewareChain(0, res);
                     } else {
                       _server._logger->error("Error reading request: " + ec.message());
                     }
                   });
}

// Recursively process the middleware chain.
// If a middleware calls next(false), the chain stops and the response is sent.
void HttpSession::processMiddlewareChain(std::size_t index,
                                           std::shared_ptr<http::response<http::string_body>> res) {
  if (index < _server.middlewares.size()) {
    auto next = [this, index, res](bool continueChain) {
      if (continueChain) {
        processMiddlewareChain(index + 1, res);
      } else {
        res->prepare_payload();
        // Middleware has halted further processing; send the response.
        do_write(res);
      }
    };
    // Invoke the middleware at the current index.
    _server.middlewares[index](_req, res, next);
  } else {
    // All middleware have been processed; send the response.
    res->prepare_payload();
    do_write(res);
  }
}

// Asynchronously write the response and shutdown the socket.
void HttpSession::do_write(std::shared_ptr<http::response<http::string_body>> res) {
  auto self = shared_from_this();
  http::async_write(_socket, *res,
                    [this, self, res](boost::system::error_code ec, std::size_t) {
                      _socket.shutdown(tcp::socket::shutdown_send, ec);
                      if (ec) {
                        _server._logger->error("Error writing response: " + ec.message());
                      }
                    });
}

}  // namespace api
