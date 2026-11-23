//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "resp.h"

namespace athenasip::datastores::resp {

// One connection to a Redis server, written by hand in place of boost::redis (decided 2026-11-23): TCP, or TLS that
// checks the server's certificate and name; AUTH and SELECT before anything else; commands pipelined and answered in
// order; a deadline on every request; and a reconnect with backoff and jitter when the link drops.
//
// Everything runs on the io_context it is given, which runs on one thread; exec() may be called from any thread.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  struct Settings {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string username;  // empty or "default" sends AUTH with the password alone
    std::string password;
    std::int32_t database = 0;
    bool tls = false;
    std::string ca_file;  // for TLS: more CA certificates to trust beside the system's

    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{5000};
    std::chrono::milliseconds reconnect_min{500};
    std::chrono::milliseconds reconnect_max{10000};

    // Requests held while the link is down, beyond which a new one fails at once.
    std::size_t max_waiting = 10000;
  };

  // A request's answer: one reply per command, or an error with what happened. An error reply from the server is a
  // reply, not an error here.
  using Handler = std::function<void(boost::system::error_code, std::string what, std::vector<Reply>)>;

  Connection(boost::asio::io_context& io, std::shared_ptr<loggers::Logger> logger, Settings settings);
  ~Connection();

  // Begins connecting; keeps reconnecting until stop().
  void start();

  // Fails every request not yet answered with operation_aborted and stops reconnecting.
  void stop();

  // Sends the request when the link is up, and answers within the request timeout.
  void exec(Request request, Handler handler);

  // Connected, signed in and selected.
  bool ready() const { return _state == State::Ready; }

 private:
  enum class State { Stopped, Connecting, Handshaking, Ready, Waiting };

  struct Pending {
    Request request;
    Handler handler;
    std::shared_ptr<boost::asio::steady_timer> deadline;
    std::vector<Reply> replies;
    bool answered = false;  // timed out or failed: its replies, when they come, are read and dropped
    bool internal = false;  // AUTH and SELECT, ahead of everything
  };
  using PendingPtr = std::shared_ptr<Pending>;

  void _connect();
  void _on_connected();
  void _handshake();
  void _on_ready();
  void _write();
  void _read();
  void _on_reply(Reply reply);
  void _lost(const std::string& why);
  void _schedule_reconnect();
  void _close_transport();
  void _answer(const PendingPtr& pending, boost::system::error_code ec, const std::string& what);
  void _arm_deadline(const PendingPtr& pending);

  template <typename Buffers, typename Completion>
  void _async_write(const Buffers& buffers, Completion&& handler);
  template <typename Buffers, typename Completion>
  void _async_read_some(const Buffers& buffers, Completion&& handler);

  boost::asio::io_context& _io;
  std::shared_ptr<loggers::LoggerScoped> _logger;
  Settings _settings;

  State _state = State::Stopped;
  std::uint64_t _generation = 0;  // one per transport, so a late completion from an old one is ignored

  boost::asio::ip::tcp::resolver _resolver;
  boost::asio::ssl::context _tls_context;
  std::unique_ptr<boost::asio::ip::tcp::socket> _socket;
  std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _tls;
  boost::asio::steady_timer _connect_timer;
  boost::asio::steady_timer _reconnect_timer;
  std::chrono::milliseconds _backoff;
  std::mt19937 _jitter{std::random_device{}()};

  std::deque<PendingPtr> _waiting;   // not yet written
  std::deque<PendingPtr> _inflight;  // written, answered in order
  bool _writing = false;
  std::string _out;
  std::vector<char> _in;
  Decoder _decoder;
};

}  // namespace athenasip::datastores::resp
