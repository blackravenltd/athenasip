//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A loopback HTTP/2 server over TLS for the push tests, on an ephemeral port with the
// repository's snakeoil certificate (signed by tls/ca/snakeca.crt), built on nghttp2's
// server session. It records every request with the connection it came on, and answers
// each with whatever the test's responder says.
class TestHttp2Server {
 public:
  struct Request {
    std::string method;
    std::string path;
    std::string scheme;
    std::string authority;
    std::vector<std::pair<std::string, std::string>> headers;  // regular fields, as received
    std::string body;
    std::string server_name;  // the SNI the client sent, empty for none
    int connection = 0;       // 1 for the first connection accepted, and so on

    std::string header(const std::string& name) const {
      for (const auto& [field, value] : headers) {
        if (field.size() == name.size() &&
            std::equal(field.begin(), field.end(), name.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
          return value;
      }
      return {};
    }

    std::size_t header_count(const std::string& name) const {
      return std::count_if(headers.begin(), headers.end(), [&](const auto& field) { return field.first == name; });
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

  // speak_h2 false makes a server that agrees to no protocol by ALPN, as an HTTP/1.1 one would.
  explicit TestHttp2Server(Responder responder, bool speak_h2 = true)
      : _responder(std::move(responder)), _ssl(boost::asio::ssl::context::tls_server), _acceptor(_io) {
    _ssl.use_certificate_chain_file(cert_file());
    _ssl.use_private_key_file(key_file(), boost::asio::ssl::context::pem);
    if (speak_h2) SSL_CTX_set_alpn_select_cb(_ssl.native_handle(), select_h2, nullptr);

    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
    _port = _acceptor.local_endpoint().port();

    boost::asio::co_spawn(_io, accept(), boost::asio::detached);
    _thread = std::thread([this]() { _io.run(); });
  }

  ~TestHttp2Server() {
    boost::asio::post(_io, [this]() {
      boost::system::error_code ignored;
      _acceptor.close(ignored);
    });
    _io.stop();
    if (_thread.joinable()) _thread.join();
  }

  TestHttp2Server(const TestHttp2Server&) = delete;
  TestHttp2Server& operator=(const TestHttp2Server&) = delete;

  std::uint16_t port() const { return _port; }

  std::string url(const std::string& path, const std::string& host = "127.0.0.1") const { return "https://" + host + ":" + std::to_string(_port) + path; }

  std::vector<Request> requests() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests;
  }

  int connections() const { return _accepted; }

  // Closes every open connection without a GOAWAY, as a peer that has gone away would.
  void drop_connections() {
    std::promise<void> done;
    boost::asio::post(_io, [this, &done]() {
      for (auto& weak : _streams) {
        if (auto stream = weak.lock()) {
          boost::system::error_code ignored;
          stream->lowest_layer().close(ignored);
        }
      }
      _streams.clear();
      done.set_value();
    });
    done.get_future().wait();
  }

 private:
  using Stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

  struct Session {
    TestHttp2Server* server = nullptr;
    Request prototype;
    std::map<std::int32_t, Request> incoming;
    std::map<std::int32_t, std::pair<std::string, std::size_t>> outgoing;
    std::vector<std::int32_t> complete;
  };

  static int select_h2(SSL*, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length, void*) {
    for (unsigned int i = 0; i < in_length; i += 1u + in[i]) {
      if (in[i] == 2 && i + 3 <= in_length && std::memcmp(in + i + 1, "h2", 2) == 0) {
        *out = in + i + 1;
        *out_length = 2;
        return SSL_TLSEXT_ERR_OK;
      }
    }
    return SSL_TLSEXT_ERR_NOACK;
  }

  static int on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) session->incoming[frame->hd.stream_id] = session->prototype;
    return 0;
  }

  static int on_header(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, std::size_t name_length, const uint8_t* value,
                       std::size_t value_length, uint8_t, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    auto found = session->incoming.find(frame->hd.stream_id);
    if (found == session->incoming.end()) return 0;

    const std::string field(reinterpret_cast<const char*>(name), name_length);
    std::string text(reinterpret_cast<const char*>(value), value_length);
    auto& request = found->second;

    if (field == ":method") {
      request.method = std::move(text);
    } else if (field == ":path") {
      request.path = std::move(text);
    } else if (field == ":scheme") {
      request.scheme = std::move(text);
    } else if (field == ":authority") {
      request.authority = std::move(text);
    } else {
      request.headers.emplace_back(field, std::move(text));
    }
    return 0;
  }

  static int on_data_chunk(nghttp2_session*, uint8_t, std::int32_t stream_id, const uint8_t* data, std::size_t length, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    auto found = session->incoming.find(stream_id);
    if (found != session->incoming.end()) found->second.body.append(reinterpret_cast<const char*>(data), length);
    return 0;
  }

  static int on_frame(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
      session->complete.push_back(frame->hd.stream_id);
    return 0;
  }

  static int on_stream_close(nghttp2_session*, std::int32_t stream_id, std::uint32_t, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    session->incoming.erase(stream_id);
    session->outgoing.erase(stream_id);
    return 0;
  }

  static nghttp2_ssize read_reply(nghttp2_session* nghttp2, std::int32_t stream_id, uint8_t* buffer, std::size_t length, std::uint32_t* flags,
                                  nghttp2_data_source*, void* user_data) {
    auto* session = static_cast<Session*>(user_data);
    auto& [body, sent] = session->outgoing[stream_id];
    (void)nghttp2;
    const auto count = std::min(length, body.size() - sent);
    std::memcpy(buffer, body.data() + sent, count);
    sent += count;
    if (sent == body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(count);
  }

  void answer(nghttp2_session* nghttp2, Session& session, std::int32_t stream_id) {
    auto found = session.incoming.find(stream_id);
    if (found == session.incoming.end()) return;

    Request recorded = found->second;
    Reply reply = _responder(recorded);
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _requests.push_back(recorded);
    }
    if (reply.silent) return;

    const std::string status_name = ":status", status = std::to_string(reply.status);
    std::vector<nghttp2_nv> nv = {make_nv(status_name, status)};
    for (const auto& [name, value] : reply.headers) nv.push_back(make_nv(name, value));

    session.outgoing[stream_id] = {reply.body, 0};
    nghttp2_data_provider2 provider{};
    provider.read_callback = read_reply;
    nghttp2_submit_response2(nghttp2, stream_id, nv.data(), nv.size(), &provider);
  }

  static nghttp2_nv make_nv(const std::string& name, const std::string& value) {
    return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(), value.size(),
            NGHTTP2_NV_FLAG_NONE};
  }

  boost::asio::awaitable<void> accept() {
    for (;;) {
      boost::system::error_code ec;
      auto socket = co_await _acceptor.async_accept(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec) co_return;
      boost::asio::co_spawn(_io, serve(std::move(socket)), boost::asio::detached);
    }
  }

  boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket) {
    boost::system::error_code ec;
    auto stream = std::make_shared<Stream>(std::move(socket), _ssl);
    _streams.push_back(stream);

    co_await stream->async_handshake(boost::asio::ssl::stream_base::server, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) co_return;

    Session session;
    session.server = this;
    session.prototype.connection = ++_accepted;
    if (const char* name = SSL_get_servername(stream->native_handle(), TLSEXT_NAMETYPE_host_name)) session.prototype.server_name = name;

    nghttp2_session_callbacks* callbacks = nullptr;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);

    nghttp2_session* nghttp2 = nullptr;
    nghttp2_session_server_new(&nghttp2, callbacks, &session);
    nghttp2_session_callbacks_del(callbacks);
    std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)> owner(nghttp2, nghttp2_session_del);

    const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(nghttp2, NGHTTP2_FLAG_NONE, settings, std::size(settings));

    std::array<uint8_t, 16384> input{};
    for (;;) {
      std::string output;
      for (;;) {
        const uint8_t* data = nullptr;
        const auto length = nghttp2_session_mem_send2(nghttp2, &data);
        if (length <= 0) break;
        output.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
      }
      if (!output.empty()) {
        co_await boost::asio::async_write(*stream, boost::asio::buffer(output), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;
      }

      const auto length = co_await stream->async_read_some(boost::asio::buffer(input), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec) co_return;
      if (nghttp2_session_mem_recv2(nghttp2, input.data(), length) < 0) co_return;

      auto complete = std::move(session.complete);
      session.complete.clear();
      for (const auto stream_id : complete) answer(nghttp2, session, stream_id);
    }
  }

  Responder _responder;
  boost::asio::io_context _io;
  boost::asio::ssl::context _ssl;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::uint16_t _port = 0;
  std::thread _thread;
  std::atomic<int> _accepted{0};
  std::vector<std::weak_ptr<Stream>> _streams;

  mutable std::mutex _mutex;
  std::vector<Request> _requests;
};
