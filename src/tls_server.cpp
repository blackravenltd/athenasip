//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_server.h"

using namespace boost::asio;
using namespace boost::asio::ssl;

namespace athenasip {

TLSServer::TLSServer(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar, std::string nonce_secret, short port)
    : Server(std::make_unique<LoggerScoped>("tls_server", logger), registrar, nonce_secret),
      _port(port),
      _acceptor(_io_context, ip::tcp::endpoint(ip::tcp::v4(), port)),
      ctx(ssl::context::sslv23) {}

bool TLSServer::set_certificates(std::string cert, std::string key) {
  try {
    ctx.use_certificate_chain_file(cert);
    _logger->info("Using Certificate PEM: " + cert);
    ctx.use_private_key_file(key, ssl::context::pem);
    _logger->info("Using Key PEM: " + key);
  } catch (const std::exception& e) {
    _logger->error("Exception While Loading Certificates: " + std::string(e.what()));
    return false;
  }

  return true;
}

void TLSServer::start() {
  _logger->debug("Starting...");

  // Do the first start_accept on that thread
  boost::asio::post(_io_context, [this]() {
    _logger->info("Listening on " + std::to_string(_port) + " (TLS)");
    start_accept();
  });

  // Run the IO Context in our thread
  _thread = std::make_shared<std::thread>([this]() { _io_context.run(); });
}

void TLSServer::stop() {
  _logger->debug("Stopping...");

  // Close All Connections
  for (const auto& pair : _sessions) pair.second->close();

  // Remove all connections
  std::unique_lock<std::shared_mutex> lock(_sessions_mutex);
  _sessions.clear();
  lock.unlock();

  // Stop Thread
  if (_thread) {
    _io_context.stop();
    if (_thread->joinable()) {
      _thread->join();
      _thread.reset();
    }
    _logger->info("Stopped");
  } else {
    _logger->debug("Already Stopped");
  }
}

void TLSServer::start_accept() {
  auto new_connection = std::make_shared<ip::tcp::socket>(_io_context);
  _acceptor.async_accept(*new_connection, boost::bind(&TLSServer::_handle_accept, this, placeholders::error, new_connection));
}

void TLSServer::_handle_accept(const boost::system::error_code& error, std::shared_ptr<ip::tcp::socket> socket) {
  boost::system::error_code ec;

  if (!error) {
    // Generate new Connection
    auto ssl_socket = std::make_shared<ssl::stream<ip::tcp::socket>>(std::move(*socket), ctx);
    std::shared_ptr<Connection> new_connection = std::make_shared<TLSConnection>(ssl_socket);

    _logger->debug("Starting TLS Connection: " + new_connection->remote_endpoint_name());

    if (!new_connection->start()) {
      _logger->info("Incoming TLS Connection Error: " + new_connection->remote_endpoint_name());

      new_connection->shutdown();
      new_connection->close();
      return;
    }

    // Create TLSSession from connection
    auto new_session = std::make_shared<Session>(_logger->base_logger(), nonce_secret, new_connection);
    _logger->info("Incoming TLS Connection Accepted: " + new_connection->remote_endpoint_name());

    // Set up events
    new_session->on_start([this](std::string endpoint, std::shared_ptr<Session> session) { return register_session(endpoint, session); });
    new_session->on_close([this](std::string endpoint, std::shared_ptr<Session> session) { return unregister_session(endpoint, session); });
    new_session->on_authenticate(
        [this](std::shared_ptr<SIPIdentity> identity, std::shared_ptr<Session> session) { return _registrar->subscriber_get(identity); });
    new_session->on_register_location([this](std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Session> session) {
      return _registrar->subscriber_register(subscriber, contact);
    });

    new_session->start();
    
  } else {
    _logger->error("Incoming Connection Accept Error: " + ec.message());
  }

  // Start accepting next connection
  boost::asio::post(_io_context, [this]() { start_accept(); });
}

}  // namespace athenasip
