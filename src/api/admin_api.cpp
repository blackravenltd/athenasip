// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>

#include "admin_api.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <iostream>

#include "../loggers/logger_scoped.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using namespace athenasip::loggers;

namespace athenasip::api {

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket, std::shared_ptr<athenasip::loggers::Logger> logger)
      : _socket(std::move(socket)), _logger(std::make_shared<LoggerScoped>(_socket.remote_endpoint().address().to_string(), logger)) {}

  void start() { do_read(); }

 private:
  tcp::socket _socket;
  beast::flat_buffer _buffer;
  std::shared_ptr<athenasip::loggers::Logger> _logger;
  http::request<http::string_body> _req;

  void do_read() {
    auto self = shared_from_this();
    http::async_read(_socket, _buffer, _req, [this, self](beast::error_code ec, std::size_t) {
      if (!ec) {
        do_write();
      } else {
        _logger->error("Error reading request: " + ec.message());
      }
    });
  }

  void do_write() {
    // Create a JSON reply using Boost.JSON.
    boost::json::object json_obj;
    json_obj["message"] = "Hello from AthenaSIP";
    std::string body = boost::json::serialize(json_obj);

    auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, _req.version());
    res->set(http::field::server, "AthenaSIP");
    res->set(http::field::content_type, "application/json");
    res->body() = body;
    res->prepare_payload();

    auto self = shared_from_this();
    http::async_write(_socket, *res, [this, self, res](beast::error_code ec, std::size_t) {
      _socket.shutdown(tcp::socket::shutdown_send, ec);
      if (ec) {
        _logger->error("Error writing response: " + ec.message());
      }
    });
  }
};

AdminAPI::AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string &bind_address, unsigned short port)
    : _logger(std::make_shared<LoggerScoped>("admin_api", logger)), _bind_address(bind_address), _port(port), _io_context(), _acceptor(_io_context) {
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

AdminAPI::~AdminAPI() { stop(); }

void AdminAPI::start() {
  _logger->debug("Starting...");
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
  _logger->info("Listening on " + _bind_address + ":" + std::to_string(_port));
  _do_accept();
}

void AdminAPI::stop() {
  _logger->debug("Stopping...");
  _io_context.stop();
  if (_thread && _thread->joinable()) {
    _thread->join();
  }
  _thread.reset();
  _logger->info("Stopped");
}

void AdminAPI::_do_accept() {
  _acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<HttpSession>(std::move(socket), _logger)->start();
    } else {
      _logger->error("Accept error: " + ec.message());
    }
    // Continue accepting new connections.
    _do_accept();
  });
}

}  // namespace athenasip::api
