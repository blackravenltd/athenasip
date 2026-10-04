//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "https_client.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <cctype>

#include "../global_io_context.h"
#include "../types/url.h"
#include "build_version.h"

namespace athenasip::push {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;

// Push services answer in a few hundred bytes; anything near this is not one.
constexpr std::uint64_t kResponseLimit = 1024 * 1024;

bool equal_ignoring_case(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool is_address(const std::string& host) {
  boost::system::error_code ec;
  asio::ip::make_address(host, ec);
  return !ec;
}

struct Target {
  std::string host;
  std::string port;
  std::string host_field;
  std::string path;
};

bool parse_target(const std::string& url_string, Target& target, std::string& error) {
  const types::URL url(url_string);

  if (!url.is_valid() || !equal_ignoring_case(url.scheme, "https") || url.host.empty()) {
    error = "not an https URL: " + url_string;
    return false;
  }

  const std::uint16_t port = url.port.value_or(443);

  target.host = url.host;
  target.port = std::to_string(port);
  target.host_field = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
  if (port != 443) target.host_field += ":" + target.port;
  target.path = url.path.empty() ? "/" : url.path;
  if (!url.query.empty()) target.path += "?" + url.query;
  return true;
}

class Exchange : public std::enable_shared_from_this<Exchange> {
 public:
  Exchange(std::shared_ptr<ssl::context> context, Target target, http::request<http::string_body> request, std::chrono::milliseconds timeout,
           plugins::Executor on, plugins::Handler<HttpsResponse> handler)
      : _context(std::move(context)),
        _strand(asio::make_strand(detail::get_global_io_context())),
        _resolver(_strand),
        _stream(_strand, *_context),
        _deadline(_strand),
        _target(std::move(target)),
        _request(std::move(request)),
        _timeout(timeout),
        _on(std::move(on)),
        _handler(std::move(handler)) {}

  void start() {
    asio::dispatch(_strand, [self = shared_from_this()]() { self->begin(); });
  }

 private:
  void begin() {
    _deadline.expires_after(_timeout);
    _deadline.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (!ec) self->expire();
    });

    // RFC 6066 3: the server name is a host name, never a literal address.
    if (!is_address(_target.host) && SSL_set_tlsext_host_name(_stream.native_handle(), _target.host.c_str()) != 1) {
      return finish(plugins::Result<HttpsResponse>::failure("could not set the server name " + _target.host));
    }

    _stream.set_verify_mode(ssl::verify_peer);
    _stream.set_verify_callback(ssl::host_name_verification(_target.host));

    _resolver.async_resolve(_target.host, _target.port,
                            [self = shared_from_this()](const boost::system::error_code& ec, asio::ip::tcp::resolver::results_type results) {
                              if (ec) return self->fail("resolve " + self->_target.host, ec);
                              self->connect(std::move(results));
                            });
  }

  void connect(asio::ip::tcp::resolver::results_type results) {
    beast::get_lowest_layer(_stream).async_connect(results, [self = shared_from_this()](const boost::system::error_code& ec, const auto&) {
      if (ec) return self->fail("connect", ec);
      self->handshake();
    });
  }

  void handshake() {
    _stream.async_handshake(ssl::stream_base::client, [self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec) return self->fail("TLS handshake", ec);
      self->write();
    });
  }

  void write() {
    http::async_write(_stream, _request, [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
      if (ec) return self->fail("write", ec);
      self->read();
    });
  }

  void read() {
    _parser.body_limit(kResponseLimit);
    http::async_read(_stream, _buffer, _parser, [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
      if (ec) return self->fail("read", ec);

      auto& message = self->_parser.get();

      HttpsResponse response;
      response.status = message.result_int();
      for (const auto& field : message.base()) response.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
      response.body = std::move(message.body());

      self->finish(plugins::Result<HttpsResponse>::success(std::move(response)));
      self->shutdown();
    });
  }

  // After the answer has gone: a close_notify is courtesy, and a peer that does not
  // return one costs a second at most.
  void shutdown() {
    beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(1));
    _stream.async_shutdown([self = shared_from_this()](const boost::system::error_code&) { beast::get_lowest_layer(self->_stream).close(); });
  }

  void expire() {
    _timed_out = true;
    _resolver.cancel();
    beast::get_lowest_layer(_stream).close();
  }

  void fail(const std::string& what, const boost::system::error_code& ec) {
    finish(plugins::Result<HttpsResponse>::failure(_timed_out ? "timed out after " + std::to_string(_timeout.count()) + " ms" : what + ": " + ec.message()));
  }

  void finish(plugins::Result<HttpsResponse> result) {
    if (_done) return;
    _done = true;
    _deadline.cancel();

    if (!_handler) return;
    asio::post(_on, [handler = std::move(_handler), result = std::move(result)]() mutable { handler(std::move(result)); });
  }

  std::shared_ptr<ssl::context> _context;
  asio::strand<asio::io_context::executor_type> _strand;
  asio::ip::tcp::resolver _resolver;
  beast::ssl_stream<beast::tcp_stream> _stream;
  asio::steady_timer _deadline;
  Target _target;
  http::request<http::string_body> _request;
  std::chrono::milliseconds _timeout;
  plugins::Executor _on;
  plugins::Handler<HttpsResponse> _handler;

  beast::flat_buffer _buffer;
  http::response_parser<http::string_body> _parser;
  bool _timed_out = false;
  bool _done = false;
};

}  // namespace

std::string HttpsResponse::header(std::string_view name) const {
  for (const auto& [field, value] : headers) {
    if (equal_ignoring_case(field, name)) return value;
  }
  return {};
}

HttpsClient::HttpsClient(std::shared_ptr<loggers::Logger> logger, Options options)
    : _logger(std::make_shared<loggers::LoggerScoped>("https", std::move(logger))),
      _options(std::move(options)),
      _context(std::make_shared<ssl::context>(ssl::context::tls_client)) {
  _context->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                        ssl::context::no_tlsv1_1);

  boost::system::error_code ec;
  _context->set_default_verify_paths(ec);
  if (ec) _logger->warn("no system certificate store: " + ec.message());

  if (!_options.ca_file.empty()) {
    _context->load_verify_file(_options.ca_file, ec);
    if (ec) _error = "could not load the CA file " + _options.ca_file + ": " + ec.message();
  }
}

void HttpsClient::post(plugins::Executor on, const std::string& url, HttpsHeaders headers, std::string body, plugins::Handler<HttpsResponse> handler) {
  Target target;
  std::string error = _error;

  if (error.empty()) parse_target(url, target, error);

  if (!error.empty()) {
    if (handler) asio::post(on, [handler = std::move(handler), error]() { handler(plugins::Result<HttpsResponse>::failure(error)); });
    return;
  }

  http::request<http::string_body> request{http::verb::post, target.path, 11};
  request.set(http::field::host, target.host_field);
  request.set(http::field::user_agent, "AthenaSIP/" ATHENA_VERSION_STRING);
  request.set(http::field::connection, "close");
  for (auto& [name, value] : headers) request.set(name, value);
  request.body() = std::move(body);
  request.prepare_payload();

  std::make_shared<Exchange>(_context, std::move(target), std::move(request), _options.timeout, std::move(on), std::move(handler))->start();
}

}  // namespace athenasip::push
