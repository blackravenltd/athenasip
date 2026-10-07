//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/ssl.h>

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A loopback HTTPS/1.1 server for the push tests, on an ephemeral port with the
// repository's snakeoil certificate (signed by tls/ca/snakeca.crt). It records every
// request and answers each with whatever the test's responder says.
class TestHttpsServer {
 public:
  struct Request {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string server_name;  // the SNI the client sent, empty for none

    std::string header(const std::string& name) const {
      for (const auto& [field, value] : headers) {
        if (field.size() == name.size() &&
            std::equal(field.begin(), field.end(), name.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
          return value;
      }
      return {};
    }

    std::size_t header_count(const std::string& name) const {
      std::size_t found = 0;
      for (const auto& [field, value] : headers) {
        std::string lower = field;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        std::string wanted = name;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == wanted) ++found;
      }
      return found;
    }
  };

  struct Reply {
    unsigned status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool silent = false;  // read the request and never answer
  };

  using Responder = std::function<Reply(const Request&)>;

  static std::string cert_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.cer"; }
  static std::string key_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.key"; }
  static std::string ca_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/ca/snakeca.crt"; }

  explicit TestHttpsServer(Responder responder) : _responder(std::move(responder)), _ssl(boost::asio::ssl::context::tls_server), _acceptor(_io) {
    _ssl.use_certificate_chain_file(cert_file());
    _ssl.use_private_key_file(key_file(), boost::asio::ssl::context::pem);

    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
    _port = _acceptor.local_endpoint().port();

    boost::asio::co_spawn(_io, accept(), boost::asio::detached);
    _thread = std::thread([this]() { _io.run(); });
  }

  ~TestHttpsServer() {
    boost::asio::post(_io, [this]() {
      boost::system::error_code ignored;
      _acceptor.close(ignored);
    });
    _io.stop();
    if (_thread.joinable()) _thread.join();
  }

  TestHttpsServer(const TestHttpsServer&) = delete;
  TestHttpsServer& operator=(const TestHttpsServer&) = delete;

  std::uint16_t port() const { return _port; }

  std::string url(const std::string& path, const std::string& host = "127.0.0.1") const { return "https://" + host + ":" + std::to_string(_port) + path; }

  std::vector<Request> requests() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests;
  }

  std::size_t count(const std::string& target) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return std::count_if(_requests.begin(), _requests.end(), [&](const Request& request) { return request.target == target; });
  }

 private:
  boost::asio::awaitable<void> accept() {
    for (;;) {
      boost::system::error_code ec;
      auto socket = co_await _acceptor.async_accept(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec) co_return;
      boost::asio::co_spawn(_io, serve(std::move(socket)), boost::asio::detached);
    }
  }

  boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    boost::system::error_code ec;
    beast::ssl_stream<beast::tcp_stream> stream(beast::tcp_stream(std::move(socket)), _ssl);

    co_await stream.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) co_return;

    Request recorded;
    if (const char* name = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name)) recorded.server_name = name;

    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    co_await http::async_read(stream, buffer, request, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) co_return;

    recorded.method = std::string(request.method_string());
    recorded.target = std::string(request.target());
    for (const auto& field : request.base()) recorded.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
    recorded.body = request.body();

    Reply reply = _responder(recorded);

    {
      std::lock_guard<std::mutex> lock(_mutex);
      _requests.push_back(recorded);
    }

    if (reply.silent) {
      boost::asio::steady_timer hold(_io, std::chrono::seconds(30));
      co_await hold.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      co_return;
    }

    http::response<http::string_body> response{static_cast<http::status>(reply.status), 11};
    for (const auto& [name, value] : reply.headers) response.set(name, value);
    response.body() = reply.body;
    response.keep_alive(false);
    response.prepare_payload();

    co_await http::async_write(stream, response, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) co_return;

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(1));
    co_await stream.async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
  }

  Responder _responder;
  boost::asio::io_context _io;
  boost::asio::ssl::context _ssl;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::uint16_t _port = 0;
  std::thread _thread;

  mutable std::mutex _mutex;
  std::vector<Request> _requests;
};
