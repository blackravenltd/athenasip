//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "http2_client.h"

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "../global_io_context.h"
#include "../types/url.h"
#include "build_version.h"

namespace athenasip::push {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;

// Push services answer in a few hundred bytes; anything near this is not one.
constexpr std::size_t kResponseLimit = 1024 * 1024;

// RFC 7301 3.1: the protocol list, each name preceded by its length.
constexpr unsigned char kAlpn[] = {2, 'h', '2'};

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
  std::string authority;
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
  target.authority = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
  if (port != 443) target.authority += ":" + target.port;
  target.path = url.path.empty() ? "/" : url.path;
  if (!url.query.empty()) target.path += "?" + url.query;
  return true;
}

nghttp2_nv header(const std::string& name, const std::string& value) {
  return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(), value.size(),
          NGHTTP2_NV_FLAG_NONE};
}

struct Request {
  Target target;
  std::chrono::steady_clock::time_point expires_at;
  std::chrono::milliseconds timeout{0};
  HttpsHeaders headers;
  std::string body;
  std::size_t sent = 0;
  plugins::Executor on;
  plugins::Handler<HttpsResponse> handler;

  HttpsResponse response;
  std::string failure;  // set when the stream is reset on our side
  std::int32_t stream_id = 0;
  std::unique_ptr<asio::steady_timer> deadline;
  bool retried = false;
  bool done = false;
};

}  // namespace

// The connections, one per origin, shared with each connection so that a request a lost
// connection never answered can be given to the next.
class Http2Client::Pool : public std::enable_shared_from_this<Pool> {
 public:
  Pool(std::shared_ptr<loggers::LoggerScoped> logger, std::shared_ptr<ssl::context> context) : _logger(std::move(logger)), _context(std::move(context)) {}

  void submit(std::shared_ptr<Request> request);
  void close();

 private:
  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<ssl::context> _context;

  std::mutex _mutex;
  std::map<std::string, std::shared_ptr<Connection>> _connections;
};

class Http2Client::Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(std::shared_ptr<loggers::LoggerScoped> logger, std::shared_ptr<ssl::context> context, std::weak_ptr<Pool> pool, Target target,
             std::chrono::milliseconds timeout)
      : _logger(std::move(logger)),
        _context(std::move(context)),
        _pool(std::move(pool)),
        _strand(asio::make_strand(detail::get_global_io_context())),
        _resolver(_strand),
        _stream(_strand, *_context),
        _connect_deadline(_strand),
        _target(std::move(target)),
        _timeout(timeout) {}

  ~Connection() {
    if (_session) nghttp2_session_del(_session);
  }

  // Whether a new request may go on this connection: false once it has closed, or the
  // server has said with GOAWAY that it takes no more streams.
  bool usable() const { return !_closing; }

  void submit(std::shared_ptr<Request> request) {
    asio::dispatch(_strand, [self = shared_from_this(), request = std::move(request)]() mutable { self->begin(std::move(request)); });
  }

  void close() {
    asio::dispatch(_strand, [self = shared_from_this()]() { self->shut("connection closed", false); });
  }

 private:
  enum class State { Idle, Connecting, Open, Closed };

  void begin(std::shared_ptr<Request> request) {
    // It went between the pool choosing it and the request arriving.
    if (_state == State::Closed) return retry(request, _close_reason);

    request->deadline = std::make_unique<asio::steady_timer>(_strand, request->expires_at);
    request->deadline->async_wait([self = shared_from_this(), weak = std::weak_ptr<Request>(request)](const boost::system::error_code& ec) {
      if (auto request = weak.lock(); request && !ec) self->expire(request);
    });

    if (_state == State::Open) {
      start_stream(request);
      return flush();
    }

    _pending.push_back(std::move(request));
    if (_state == State::Idle) open();
  }

  void open() {
    _state = State::Connecting;

    _connect_deadline.expires_after(_timeout);
    _connect_deadline.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (!ec && self->_state == State::Connecting) self->shut("timed out connecting to " + self->_target.authority, false);
    });

    // RFC 6066 3: the server name is a host name, never a literal address.
    if (!is_address(_target.host) && SSL_set_tlsext_host_name(_stream.native_handle(), _target.host.c_str()) != 1) {
      return shut("could not set the server name " + _target.host, false);
    }

    // RFC 9113 3.2: HTTP/2 over TLS is chosen by ALPN, and nothing else is offered.
    if (SSL_set_alpn_protos(_stream.native_handle(), kAlpn, sizeof(kAlpn)) != 0) return shut("could not offer h2 by ALPN", false);

    _stream.set_verify_mode(ssl::verify_peer);
    _stream.set_verify_callback(ssl::host_name_verification(_target.host));

    _resolver.async_resolve(_target.host, _target.port,
                            [self = shared_from_this()](const boost::system::error_code& ec, asio::ip::tcp::resolver::results_type results) {
                              if (ec) return self->fail("resolve " + self->_target.host, ec);
                              asio::async_connect(self->_stream.lowest_layer(), results, [self](const boost::system::error_code& ec, const auto&) {
                                if (ec) return self->fail("connect", ec);
                                self->handshake();
                              });
                            });
  }

  void handshake() {
    _stream.async_handshake(ssl::stream_base::client, [self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec) return self->fail("TLS handshake", ec);

      const unsigned char* protocol = nullptr;
      unsigned int length = 0;
      SSL_get0_alpn_selected(self->_stream.native_handle(), &protocol, &length);
      if (length != 2 || std::memcmp(protocol, "h2", 2) != 0) return self->shut(self->_target.authority + " did not agree to HTTP/2", false);

      self->established();
    });
  }

  void established() {
    if (_state == State::Closed) return;

    nghttp2_session_callbacks* callbacks = nullptr;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    const int created = nghttp2_session_client_new(&_session, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (created != 0) return shut("could not start an HTTP/2 session", false);

    // RFC 9113 6.5.2: a client never wants server push.
    const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
    nghttp2_submit_settings(_session, NGHTTP2_FLAG_NONE, settings, std::size(settings));

    _connect_deadline.cancel();
    _state = State::Open;
    _logger->debug("HTTP/2 connection to " + _target.authority);

    auto pending = std::move(_pending);
    _pending.clear();
    for (auto& request : pending) start_stream(request);

    flush();
    read();
  }

  void start_stream(const std::shared_ptr<Request>& request) {
    const std::string method = ":method", post = "POST", scheme_name = ":scheme", scheme = "https", authority_name = ":authority", path_name = ":path";
    const std::string agent_name = "user-agent", agent = "AthenaSIP/" ATHENA_VERSION_STRING;
    const std::string length_name = "content-length", length = std::to_string(request->body.size());

    // RFC 9113 8.2.1: field names are lowercase on the wire.
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(request->headers.size());
    for (const auto& [name, value] : request->headers) {
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      fields.emplace_back(std::move(lower), value);
    }

    std::vector<nghttp2_nv> nv = {
        header(method, post),      header(scheme_name, scheme), header(authority_name, _target.authority), header(path_name, request->target.path),
        header(agent_name, agent), header(length_name, length)};
    for (const auto& [name, value] : fields) nv.push_back(header(name, value));

    nghttp2_data_provider2 provider{};
    provider.source.ptr = request.get();
    provider.read_callback = read_body;

    const auto stream_id = nghttp2_submit_request2(_session, nullptr, nv.data(), nv.size(), &provider, request.get());
    if (stream_id < 0) return finish(*request, plugins::Result<HttpsResponse>::failure(std::string("HTTP/2: ") + nghttp2_strerror(stream_id)));

    request->stream_id = stream_id;
    _streams[stream_id] = request;
  }

  void read() {
    _stream.async_read_some(asio::buffer(_input), [self = shared_from_this()](const boost::system::error_code& ec, std::size_t length) {
      if (ec) return self->fail("read", ec);

      const auto consumed = nghttp2_session_mem_recv2(self->_session, self->_input.data(), length);
      if (consumed < 0) return self->shut(std::string("HTTP/2: ") + nghttp2_strerror(static_cast<int>(consumed)), false);

      self->flush();
      if (self->_state == State::Open) self->read();
    });
  }

  void flush() {
    if (_writing || _state != State::Open) return;

    for (;;) {
      const uint8_t* data = nullptr;
      const auto length = nghttp2_session_mem_send2(_session, &data);
      if (length < 0) return shut(std::string("HTTP/2: ") + nghttp2_strerror(static_cast<int>(length)), false);
      if (length == 0) break;
      _output.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
    }

    if (_output.empty()) {
      // After GOAWAY the connection serves the streams it has, then goes.
      if ((_closing && _streams.empty()) || (!nghttp2_session_want_read(_session) && !nghttp2_session_want_write(_session))) shut("connection closed", true);
      return;
    }

    _writing = true;
    asio::async_write(_stream, asio::buffer(_output), [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
      self->_writing = false;
      self->_output.clear();
      if (ec) return self->fail("write", ec);
      self->flush();
    });
  }

  void expire(const std::shared_ptr<Request>& request) {
    if (request->done) return;
    finish(*request, plugins::Result<HttpsResponse>::failure("timed out after " + std::to_string(request->timeout.count()) + " ms"));

    // RFC 9113 8.7: the stream is cancelled, and the connection carries on.
    if (request->stream_id > 0 && _state == State::Open) {
      nghttp2_submit_rst_stream(_session, NGHTTP2_FLAG_NONE, request->stream_id, NGHTTP2_CANCEL);
      flush();
    } else {
      _pending.erase(std::remove(_pending.begin(), _pending.end(), request), _pending.end());
    }
  }

  // A connection lost once it was up may have been closed while idle, which a server is
  // free to do; what was sent on it gets one more try on a new one.
  void fail(const std::string& what, const boost::system::error_code& ec) {
    const bool was_open = _state == State::Open;
    if (ec == asio::error::eof || ec == ssl::error::stream_truncated) return shut(_target.authority + " closed the connection", was_open);
    shut(what + ": " + ec.message(), was_open);
  }

  // Closes the connection. A request on it, or waiting for it, fails with `reason`, or
  // when `retry_unanswered` goes to another connection if nothing of its answer had come.
  void shut(const std::string& reason, bool retry_unanswered) {
    if (_state == State::Closed) return;

    _state = State::Closed;
    _closing = true;
    _close_reason = reason;

    _connect_deadline.cancel();
    _resolver.cancel();
    boost::system::error_code ignored;
    _stream.lowest_layer().close(ignored);

    auto pending = std::move(_pending);
    _pending.clear();
    auto streams = std::move(_streams);
    _streams.clear();

    for (auto& request : pending) {
      if (retry_unanswered) {
        retry(request, reason);
      } else {
        finish(*request, plugins::Result<HttpsResponse>::failure(reason));
      }
    }
    for (auto& [id, request] : streams) {
      if (retry_unanswered && request->response.status == 0) {
        retry(request, reason);
      } else {
        finish(*request, plugins::Result<HttpsResponse>::failure(reason));
      }
    }
  }

  void retry(const std::shared_ptr<Request>& request, const std::string& reason) {
    auto pool = _pool.lock();
    if (!pool || request->retried || request->done || std::chrono::steady_clock::now() >= request->expires_at) {
      return finish(*request, plugins::Result<HttpsResponse>::failure(reason));
    }

    if (request->deadline) request->deadline->cancel();
    request->retried = true;
    request->stream_id = 0;
    request->sent = 0;
    request->response = {};
    pool->submit(request);
  }

  void finish(Request& request, plugins::Result<HttpsResponse> result) {
    if (request.done) return;
    request.done = true;
    if (request.deadline) request.deadline->cancel();

    if (!request.handler) return;
    asio::post(request.on, [handler = std::move(request.handler), result = std::move(result)]() mutable { handler(std::move(result)); });
  }

  static nghttp2_ssize read_body(nghttp2_session*, std::int32_t, uint8_t* buffer, std::size_t length, std::uint32_t* flags, nghttp2_data_source* source,
                                 void*) {
    auto* request = static_cast<Request*>(source->ptr);
    const auto count = std::min(length, request->body.size() - request->sent);
    std::memcpy(buffer, request->body.data() + request->sent, count);
    request->sent += count;
    if (request->sent == request->body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(count);
  }

  Request* stream(std::int32_t stream_id) {
    const auto found = _streams.find(stream_id);
    return found == _streams.end() ? nullptr : found->second.get();
  }

  static int on_header(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, std::size_t name_length, const uint8_t* value,
                       std::size_t value_length, uint8_t, void* user_data) {
    auto* self = static_cast<Connection*>(user_data);
    auto* request = self->stream(frame->hd.stream_id);
    if (!request || frame->hd.type != NGHTTP2_HEADERS) return 0;

    const std::string_view field(reinterpret_cast<const char*>(name), name_length);
    const std::string text(reinterpret_cast<const char*>(value), value_length);

    if (field == ":status") {
      unsigned status = 0;
      for (const char c : text) status = std::isdigit(static_cast<unsigned char>(c)) ? status * 10 + static_cast<unsigned>(c - '0') : 0;

      // RFC 9113 8.1: informational responses come before the final one and are dropped.
      if (status >= 200) request->response.status = status;
      request->response.headers.clear();
    } else if (!field.empty() && field.front() != ':' && request->response.status >= 200) {
      request->response.headers.emplace_back(std::string(field), text);
    }
    return 0;
  }

  static int on_data_chunk(nghttp2_session* session, uint8_t, std::int32_t stream_id, const uint8_t* data, std::size_t length, void* user_data) {
    auto* self = static_cast<Connection*>(user_data);
    auto* request = self->stream(stream_id);
    if (!request) return 0;

    if (request->response.body.size() + length > kResponseLimit) {
      request->failure = "response larger than " + std::to_string(kResponseLimit) + " bytes";
      nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
      return 0;
    }

    request->response.body.append(reinterpret_cast<const char*>(data), length);
    return 0;
  }

  static int on_frame(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Connection*>(user_data);

    // RFC 9113 6.8: no new streams; those the server has not taken are closed as refused.
    if (frame->hd.type == NGHTTP2_GOAWAY) {
      self->_closing = true;
      self->_logger->debug(self->_target.authority + " sent GOAWAY: " + nghttp2_http2_strerror(frame->goaway.error_code));
    }
    return 0;
  }

  static int on_stream_close(nghttp2_session*, std::int32_t stream_id, std::uint32_t error_code, void* user_data) {
    auto* self = static_cast<Connection*>(user_data);
    const auto found = self->_streams.find(stream_id);
    if (found == self->_streams.end()) return 0;

    auto request = std::move(found->second);
    self->_streams.erase(found);

    if (!request->failure.empty()) {
      self->finish(*request, plugins::Result<HttpsResponse>::failure(request->failure));
    } else if (error_code == NGHTTP2_REFUSED_STREAM) {
      // RFC 9113 8.7: a refused stream was not processed, and may be sent again.
      self->retry(request, "stream refused");
    } else if (error_code != NGHTTP2_NO_ERROR) {
      self->finish(*request, plugins::Result<HttpsResponse>::failure(std::string("stream reset: ") + nghttp2_http2_strerror(error_code)));
    } else if (request->response.status < 200) {
      self->finish(*request, plugins::Result<HttpsResponse>::failure("stream closed without a response"));
    } else {
      self->finish(*request, plugins::Result<HttpsResponse>::success(std::move(request->response)));
    }
    return 0;
  }

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<ssl::context> _context;
  std::weak_ptr<Pool> _pool;
  asio::strand<asio::io_context::executor_type> _strand;
  asio::ip::tcp::resolver _resolver;
  ssl::stream<asio::ip::tcp::socket> _stream;
  asio::steady_timer _connect_deadline;
  Target _target;
  std::chrono::milliseconds _timeout;

  State _state = State::Idle;
  std::atomic<bool> _closing{false};
  std::string _close_reason;
  nghttp2_session* _session = nullptr;

  std::deque<std::shared_ptr<Request>> _pending;
  std::map<std::int32_t, std::shared_ptr<Request>> _streams;

  std::array<uint8_t, 16384> _input{};
  std::string _output;
  bool _writing = false;
};

void Http2Client::Pool::submit(std::shared_ptr<Request> request) {
  std::shared_ptr<Connection> connection;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto& existing = _connections[request->target.host + ":" + request->target.port];
    if (!existing || !existing->usable()) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(request->expires_at - std::chrono::steady_clock::now());
      existing = std::make_shared<Connection>(_logger, _context, weak_from_this(), request->target, std::max(left, std::chrono::milliseconds(1)));
    }
    connection = existing;
  }

  connection->submit(std::move(request));
}

void Http2Client::Pool::close() {
  std::map<std::string, std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    connections.swap(_connections);
  }

  for (auto& [origin, connection] : connections) connection->close();
}

Http2Client::Http2Client(std::shared_ptr<loggers::Logger> logger, Options options)
    : _logger(std::make_shared<loggers::LoggerScoped>("http2", std::move(logger))), _timeout(options.timeout) {
  auto context = std::make_shared<ssl::context>(ssl::context::tls_client);

  // RFC 9113 9.2: TLS 1.2 or later.
  context->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);

  boost::system::error_code ec;
  context->set_default_verify_paths(ec);
  if (ec) _logger->warn("no system certificate store: " + ec.message());

  if (!options.ca_file.empty()) {
    context->load_verify_file(options.ca_file, ec);
    if (ec) _error = "could not load the CA file " + options.ca_file + ": " + ec.message();
  }

  _pool = std::make_shared<Pool>(_logger, std::move(context));
}

Http2Client::~Http2Client() { close(); }

void Http2Client::post(plugins::Executor on, const std::string& url, HttpsHeaders headers, std::string body, plugins::Handler<HttpsResponse> handler) {
  Target target;
  std::string error = _error;

  if (error.empty()) parse_target(url, target, error);

  if (!error.empty()) {
    if (handler) asio::post(on, [handler = std::move(handler), error]() { handler(plugins::Result<HttpsResponse>::failure(error)); });
    return;
  }

  auto request = std::make_shared<Request>();
  request->target = std::move(target);
  request->timeout = _timeout;
  request->expires_at = std::chrono::steady_clock::now() + _timeout;
  request->headers = std::move(headers);
  request->body = std::move(body);
  request->on = std::move(on);
  request->handler = std::move(handler);

  _pool->submit(std::move(request));
}

void Http2Client::close() { _pool->close(); }

}  // namespace athenasip::push
