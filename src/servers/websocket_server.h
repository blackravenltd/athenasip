//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "server.h"

namespace athenasip {
namespace servers {

// RFC 7118, over a plain socket or over TLS. One listener class rather than two: the
// only difference is a handshake before the HTTP upgrade, and a node that ran two
// near-identical listeners would drift between them.
//
// WSS is not a hardening option for a web client, it is the only way in - a page served
// over https may not open an insecure WebSocket - so ws:// is for local development and
// wss:// is what a browser actually uses.
class WebsocketServer : public Server {
 public:
  WebsocketServer(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, const std::string& bind_address, short port);

  void start() override;
  void stop() override;

  // Turns this listener into a wss:// one. Called before start(); false when the
  // certificate or the key will not load, which the caller treats as fatal rather than
  // quietly falling back to ws://.
  bool set_certificates(std::string cert, std::string key);

  // The port actually being listened on. Not the configured one when that was zero and
  // the operating system chose, which is how a test binds without picking a number that
  // something else on the machine may already hold.
  std::uint16_t port() const { return _acceptor.local_endpoint().port(); }

 protected:
  void start_accept();
  void _handle_accept(const boost::system::error_code& error, std::shared_ptr<boost::asio::ip::tcp::socket> socket);

  // The TLS handshake, and then the HTTP upgrade on the other side of it.
  void _start_tls_session(std::shared_ptr<boost::asio::ip::tcp::socket> socket);

  std::string _scheme() const { return _tls ? "wss" : "ws"; }

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::shared_ptr<std::thread> _thread;

  bool _tls = false;
  boost::asio::ssl::context _ssl_context;
};

}  // namespace servers
}  // namespace athenasip
