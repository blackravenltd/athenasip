//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_session.h"

namespace athenasip {

TLSSession::TLSSession(std::shared_ptr<Logger> logger, TLSServer *server, std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection)
    : _connection(connection), _server(server), _running(false), _rx_timer(_rx_wait_context, boost::asio::chrono::seconds(10)) {
  auto rep = _connection->lowest_layer().remote_endpoint();
  remote_endpoint = rep.address().to_string() + ":" + std::to_string(rep.port());

  _logger = std::make_unique<LoggerScoped>(remote_endpoint, logger);
  _thread = std::make_unique<std::thread>(std::bind(&TLSSession::_execute, this, 0));
}

void TLSSession::close() {
  if (_running) {
    // Signal Thread
    _running = false;

    // Cancel reading
    _rx_timer.cancel();
    _ssl_close();
  }

  // Wait for thread quit, but avoid joining the current thread
  if (_thread && _thread->get_id() != std::this_thread::get_id() && _thread->joinable()) {
    _thread->join();
  }
}

void TLSSession::_execute(int id) {
  _running = true;

  _logger->info("Connected");

  char inputbuffer[65535];
  std::string buffer;

  while (_running) {
    boost::system::error_code ec;

    ssize_t len = _read_with_timeout(inputbuffer, 65535, 5000, ec);

    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        // Timed out, normal operation
      } else if (ec == boost::asio::error::eof) {
        _logger->info("Remote Closed Connection");
        _running = false;
      } else if (ec == boost::asio::ssl::error::stream_truncated) {
        _logger->info("Remote Terminated Connection");
        _running = false;
      } else {
        _logger->error("Closing (Error during read: " + ec.what() + ")");
        _running = false;
      }
    } else {
      // No Input
      if (len == 0) continue;
      _logger->debug("Read " + std::to_string(len) + " bytes");

      // Add to buffer
      buffer.append(inputbuffer, len);

      // Look for CRLFCRLF
      size_t pos;
      while ((pos = buffer.find("\r\n\r\n")) != std::string::npos) {
        std::string sip_message = buffer.substr(0, pos + 2);
        buffer.erase(0, pos + 4);

        SIPHeader header(sip_message);
        header.print();
      }
    }
  }

  _logger->info("Closed");
}

ssize_t TLSSession::_read_with_timeout(void *ptr, size_t len, uint32_t timeout_ms, boost::system::error_code &ec) {
  if (!ptr || len == 0) return -1;  // Validate input parameters

  ssize_t rec_len = 0;

  // Set the timeout
  _rx_timer.expires_after(boost::asio::chrono::milliseconds(timeout_ms));

  // Do the async read
  _connection->async_read_some(boost::asio::buffer(ptr, len), [&](const boost::system::error_code &error, std::size_t length) {
    if (!error) {
      rec_len = length;
    } else {
      rec_len = -1;
    }
    ec = error;
    _rx_timer.cancel();
  });

  // Set up the asynchronous wait on the _rx_timer
  _rx_timer.async_wait([&](const boost::system::error_code &error) {
    if (error) {
      if (error == boost::asio::error::operation_aborted) {
        // Normal operation.
      }
    } else {
      ec = boost::asio::error::operation_aborted;
    }
  });

  // Wait for the timeout, or read to complete
  _rx_wait_context.run();
  _rx_wait_context.restart();

  return static_cast<ssize_t>(rec_len);  // Successfully read 'length' bytes
}

void TLSSession::_ssl_close() {
  boost::system::error_code ec;

  if (_connection->lowest_layer().is_open()) {
    // SSL
    _connection->shutdown(ec);
    // Underlying TCP
    _connection->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    _connection->lowest_layer().close(ec);
  }
}

}  // namespace athenasip