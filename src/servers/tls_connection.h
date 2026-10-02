//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <cstddef>
#include <functional>
#include <memory>

#include "connection.h"

using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip::servers {

class TLSConnection : public Connection {
 public:
  // handshaken is for a flow this node opened, whose client handshake has already run.
  TLSConnection(std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> ssl_socket, bool handshaken = false)
      : _ssl_socket(ssl_socket),
        _local_endpoint(_ssl_socket->lowest_layer().local_endpoint()),
        _remote_endpoint(_ssl_socket->lowest_layer().remote_endpoint()),
        _handshaken(handshaken) {
    if (_handshaken) _read_peer();
  }

  virtual bool start() override {
    if (_handshaken) return true;

    try {
      _ssl_socket->handshake(ssl::stream_base::server);
    } catch (const std::exception& e) {
      return false;
    }

    _handshaken = true;
    _read_peer();
    return true;
  }

  std::string peer_identity() const override { return _peer; }

  boost::asio::any_io_executor executor() override { return _ssl_socket->lowest_layer().get_executor(); }

  virtual void async_read_some(boost::asio::mutable_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _ssl_socket->async_read_some(buffer, [this, handler](boost::system::error_code ec, std::size_t length) {
      if (ec == boost::asio::ssl::error::stream_truncated) {
        ec = boost::asio::error::operation_aborted;
      }
      handler(ec, length);
    });
  }

  virtual void async_write_some(boost::asio::const_buffer buffer, std::function<void(const boost::system::error_code&, std::size_t)> handler) override {
    _ssl_socket->async_write_some(buffer, [this, handler](boost::system::error_code ec, std::size_t length) { handler(ec, length); });
  }

  virtual boost::asio::ip::tcp::endpoint local_endpoint() override { return _local_endpoint; }
  virtual boost::asio::ip::tcp::endpoint remote_endpoint() override { return _remote_endpoint; }

  virtual bool is_open() override { return _ssl_socket->lowest_layer().is_open(); }

  virtual bool is_reliable() override { return true; }

  virtual void shutdown() override {
    boost::system::error_code ec;
    _ssl_socket->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ec);
  }

  virtual void close() override { _ssl_socket->lowest_layer().close(); }

  virtual std::string transport_name() const override { return "tls"; }

 protected:
  // The verified peer certificate's common name, when the context asked for one.
  void _read_peer() {
    X509* certificate = SSL_get1_peer_certificate(_ssl_socket->native_handle());
    if (certificate == nullptr) return;

    if (SSL_get_verify_result(_ssl_socket->native_handle()) == X509_V_OK) {
      char name[256] = {};
      if (X509_NAME_get_text_by_NID(X509_get_subject_name(certificate), NID_commonName, name, sizeof(name)) > 0) _peer = name;
    }
    X509_free(certificate);
  }

  std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _ssl_socket;
  boost::asio::ip::tcp::endpoint _local_endpoint;
  boost::asio::ip::tcp::endpoint _remote_endpoint;
  bool _handshaken = false;
  std::string _peer;
};

}  // namespace athenasip::servers
