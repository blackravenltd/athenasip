//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "tls_session.h"

namespace athenasip {

TLSSession::TLSSession(std::shared_ptr<Logger> logger, TLSServer *server, std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> connection)
    : _connection(connection), _server(server) {
  auto rep = _connection->lowest_layer().remote_endpoint();
  remote_endpoint = rep.address().to_string() + ":" + std::to_string(rep.port());

  _logger = std::make_unique<LoggerScoped>(remote_endpoint, logger);
}

void TLSSession::close() {
  auto self(shared_from_this());
  
  boost::system::error_code ec;

  // Register Timeout
  if (_register_timeout) {
    _register_timeout->cancel();
    _register_timeout.reset();
  }

  // Ensure Connection Closed
  if (_connection && _connection->lowest_layer().is_open()) {
    _connection->shutdown(ec);
    _connection->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    _connection->lowest_layer().close(ec);

    _connection.reset();
    _logger->debug("Closed");
    
    _server->unregister_connection(shared_from_this());
  }
}

void TLSSession::write(std::string message) {
  auto self(shared_from_this());

  boost::asio::async_write(*_connection, boost::asio::buffer(message), [this, self](boost::system::error_code ec, std::size_t) {
    if (ec) {
      _logger->error("Write Error ");
      close();
    }
  });
}

void TLSSession::_start() {
  auto self(shared_from_this());

  _logger->info("Connected");

  // REGISTER timeout.
  _register_timeout = DelayedTask<int>::schedule(
      [this, self] {
        _logger->info("Exceeded REGISTER Timeout (5000ms)");

        write(
            "SIP/2.0 408 Request Timeout\r\nVia: SIP/2.0/TLS client.example.com;branch=z9hG4bK776asdhds\r\nFrom: <sip:user@example.com>;tag=123456\r\nTo: "
            "<sip:server@example.com>\r\nCall-ID: abc123@example.com\r\nCSeq: 1 REGISTER\r\nContent-Length: 0\r\n\r\n");

        close();
        return 0;
      },
      5000);

  _schedule_async_read();
}

void TLSSession::_schedule_async_read() {
  auto self(shared_from_this());

  _connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
    if (ec) {
      if (ec == boost::asio::error::operation_aborted || ec == boost::asio::error::eof) {
        _logger->info("Closed Connection");
      } else if (ec == boost::asio::ssl::error::stream_truncated) {
        _logger->info("Remote Terminated Connection");
      } else {
        _logger->error("Error during read: " + ec.what());
      }
      close();
    } else {
      // No Input, schedule read again and exit
      if (length == 0) {
        _schedule_async_read();
        return;
      };

      // Read complete
      _logger->debug("Read " + std::to_string(length) + " bytes");

      // Add to buffer
      _buffer.append(_read_buffer.data(), length);

      // Reject Crap (No register, more than 16k data sent)
      if(state == State::Initial && _buffer.size()>65535) {
        _logger->info("Client is babbling, closing (no rational input for more than 65535 bytes)");
        write("SIP/2.0 400 Bad Request\r\nVia: SIP/2.0/UDP 192.168.1.100:5060;branch=z9hG4bK\r\nContent-Length: 0\r\n\r\n");
        close();
        return;
      }

      // Process messages
      size_t pos;
      while ((pos = _buffer.find("\r\n\r\n")) != std::string::npos) {
        std::string sip_message = _buffer.substr(0, pos + 2);
        _buffer.erase(0, pos + 4);

        SIPHeader header(sip_message);
        header.print();
      }

      // Schedule Next Read
      _schedule_async_read();
    }
  });
}
}  // namespace athenasip