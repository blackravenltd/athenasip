//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "admin_api.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "../loggers/logger_scoped.h"
#include "../servers/tls_context.h"

using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

namespace athenasip::api {

// One request and its response, over a plain socket or a TLS stream.
template <typename Stream>
class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>> {
 public:
  HttpSession(Stream stream, AdminAPI& server) : _stream(std::move(stream)), _server(server) {
    boost::system::error_code ec;
    auto rm = beast::get_lowest_layer(_stream).remote_endpoint(ec);
    _remote = ec ? std::string() : rm.address().to_string();
    _logger = std::make_shared<loggers::LoggerScoped>(ec ? std::string("unknown") : _remote + ":" + std::to_string(rm.port()), server._logger);
  }

  void start() { _start(_stream); }

 private:
  std::shared_ptr<athenasip::loggers::Logger> _logger;

  Stream _stream;
  beast::flat_buffer _buffer;
  AdminAPI& _server;
  http::request<http::string_body> _req;
  std::string _remote;

  // Bounds the TLS handshake at ten seconds, as the SIP TLS listener does.
  net::steady_timer _deadline{_server._io_context};

  void _start(tcp::socket&) { do_read(); }

  void _start(net::ssl::stream<tcp::socket>& stream) {
    auto self = this->shared_from_this();

    _deadline.expires_after(std::chrono::seconds(10));
    _deadline.async_wait([this, self](boost::system::error_code ec) {
      if (ec) return;
      boost::system::error_code ignored;
      beast::get_lowest_layer(_stream).close(ignored);
    });

    stream.async_handshake(net::ssl::stream_base::server, [this, self](boost::system::error_code ec) {
      _deadline.cancel();
      if (ec) {
        _logger->debug("TLS handshake failed: " + ec.message());
        return;
      }
      do_read();
    });
  }

  void do_read() {
    auto self = this->shared_from_this();
    http::async_read(_stream, _buffer, _req, [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
      (void)bytes_transferred;  // Unused
      if (!ec) {
        _logger->debug(std::string(http::to_string(_req.method())) + " " + std::string(_req.target()));
        auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, _req.version());
        process_middleware_chain(0, res);
      } else {
        _server._logger->error("Error reading request: " + ec.message());
      }
    });
  }

  // Runs the middleware at index; next(true) moves on, next(false) sends the response.
  void process_middleware_chain(std::size_t index, std::shared_ptr<http::response<http::string_body>> res) {
    if (index < _server.middlewares.size()) {
      // self keeps the session alive while a middleware answers asynchronously.
      auto self = this->shared_from_this();

      auto next = [this, self, index, res](bool continueChain) {
        if (continueChain) {
          process_middleware_chain(index + 1, res);
        } else {
          res->prepare_payload();
          do_write(res);
        }
      };
      _server.middlewares[index](_req, _remote, res, next);
    } else {
      res->prepare_payload();
      do_write(res);
    }
  }

  void do_write(std::shared_ptr<http::response<http::string_body>> res) {
    auto self = this->shared_from_this();
    http::async_write(_stream, *res, [this, self, res](boost::system::error_code ec, std::size_t) {
      if (ec) _server._logger->error("Error writing response: " + ec.message());
      _finish(_stream);
    });
  }

  void _finish(tcp::socket& socket) {
    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_send, ec);
  }

  // TLS closes with close_notify, so a client can tell a complete response from a
  // truncated one.
  void _finish(net::ssl::stream<tcp::socket>& stream) {
    auto self = this->shared_from_this();
    stream.async_shutdown([this, self](boost::system::error_code) {
      boost::system::error_code ec;
      beast::get_lowest_layer(_stream).shutdown(tcp::socket::shutdown_send, ec);
    });
  }
};

AdminAPI::AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string& bind_address, unsigned short port)
    : _logger(std::make_shared<loggers::LoggerScoped>("admin_api", logger)),
      _bind_address(bind_address),
      _port(port),
      _io_context(),
      _acceptor(_io_context),
      _tls_acceptor(_io_context) {
  _listen(_acceptor, _bind_address, _port, *_logger);
}

bool AdminAPI::_listen(tcp::acceptor& acceptor, const std::string& bind_address, unsigned short port, athenasip::loggers::Logger& logger) {
  boost::system::error_code ec;
  tcp::endpoint endpoint(net::ip::make_address(bind_address, ec), port);
  if (ec) {
    logger.error("Failed to create endpoint: " + ec.message());
    return false;
  }
  acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    logger.error("Failed to open acceptor: " + ec.message());
    return false;
  }
  acceptor.set_option(net::socket_base::reuse_address(true), ec);
  if (ec) {
    logger.error("Failed to set reuse address: " + ec.message());
    return false;
  }
  acceptor.bind(endpoint, ec);
  if (ec) {
    logger.error("Failed to bind acceptor: " + ec.message());
    return false;
  }
  acceptor.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    logger.error("Failed to listen on acceptor: " + ec.message());
    return false;
  }
  return true;
}

bool AdminAPI::tls_enable(const std::string& bind_address, unsigned short port, const std::string& cert, const std::string& key) {
  auto context = std::make_shared<net::ssl::context>(net::ssl::context::tls_server);

  // The loader every secure listener uses, so the same protocol versions are refused.
  if (!servers::load_tls_certificates(_logger, *context, cert, key)) return false;
  if (!_listen(_tls_acceptor, bind_address, port, *_logger)) return false;

  _tls_context = std::move(context);
  _tls_bind_address = bind_address;
  return true;
}

AdminAPI::~AdminAPI() { stop(); }

std::uint16_t AdminAPI::port() const {
  boost::system::error_code ec;
  const auto endpoint = _acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

std::uint16_t AdminAPI::tls_port() const {
  if (!_tls_context) return 0;

  boost::system::error_code ec;
  const auto endpoint = _tls_acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

void AdminAPI::start() {
  _logger->debug("Starting AdminAPI server...");
  _do_accept();
  if (_tls_context) _do_accept_tls();
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
  _logger->info("Listening on " + _bind_address + ":" + std::to_string(_port));
  if (_tls_context) _logger->info("Listening for HTTPS on " + _tls_bind_address + ":" + std::to_string(tls_port()));
}

void AdminAPI::stop() {
  _logger->debug("Stopping AdminAPI server...");
  _io_context.stop();
  if (_thread && _thread->joinable()) {
    _thread->join();
  }
  _thread.reset();
  _logger->info("AdminAPI server stopped.");
}

void AdminAPI::_do_accept() {
  _acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<HttpSession<tcp::socket>>(std::move(socket), *this)->start();
    } else {
      _logger->error("Accept error: " + ec.message());
    }
    _do_accept();
  });
}

// The handshake is the session's and asynchronous, so a connection that never finishes
// one holds up nobody else.
void AdminAPI::_do_accept_tls() {
  _tls_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<HttpSession<net::ssl::stream<tcp::socket>>>(net::ssl::stream<tcp::socket>(std::move(socket), *_tls_context), *this)->start();
    } else {
      _logger->error("Accept error: " + ec.message());
    }
    _do_accept_tls();
  });
}

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

}  // namespace athenasip::api
