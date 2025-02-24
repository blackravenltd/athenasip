//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <boost/asio.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

using namespace athenasip::loggers;

namespace athenasip::rtp {

class RTPRelayPair : public std::enable_shared_from_this<RTPRelayPair> {
 public:
  std::string bind_address;

  bool remote_a_registered;
  bool remote_b_registered;

  boost::asio::ip::udp::endpoint remote_a;
  boost::asio::ip::udp::endpoint remote_b;

  uint16_t port_a;
  uint16_t port_b;

  RTPRelayPair(std::shared_ptr<Logger> logger, const std::string &_bind_address, uint16_t _port_a, uint16_t _port_b)
      : _logger(std::make_shared<LoggerScoped>(bind_address + ":" + std::to_string(_port_a) + "<->" + std::to_string(_port_b), logger)),
        port_a(_port_a),
        port_b(_port_b),
        bind_address(_bind_address),
        _io_context(detail::getGlobalIOContext()),
        _socket_a(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(bind_address), _port_a)),
        _socket_b(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(bind_address), _port_b)),
        _buffer_a(boost::asio::buffer(_recv_buffer_a)),
        _buffer_b(boost::asio::buffer(_recv_buffer_b)) {}

  void start() {
    read_a();
    read_b();
    _logger->info("Started on " + bind_address);
  }

  void stop() {
    _socket_a.close();
    _socket_b.close();
    _logger->info("Stopped");
  }

  void read_a() {
    auto self = shared_from_this();

    boost::asio::ip::udp::endpoint sender_endpoint;

    _socket_a.async_receive_from(
        boost::asio::buffer(_buffer_a), sender_endpoint, [this, self, &sender_endpoint](boost::system::error_code ec, std::size_t bytes_recvd) {
          if (ec == boost::asio::error::operation_aborted) {
            // Socket shutdown. Return without scheduling more reads
            return;
          } else if (ec) {
            _logger->error("Socket A error: " + ec.what());
          }

          // Record remote sender
          if (!remote_a_registered) {
            remote_a = sender_endpoint;
            remote_a_registered = true;
            _logger->debug("A side endpoint " + remote_a.address().to_string() + ":" + std::to_string(remote_a.port()));
          }

          // TODO: Implement Queueing
          // Drop if error, zero length, or other side has not registered
          if (ec || bytes_recvd == 0 || !remote_b_registered) {
            // Ignore errors, packets not from remote_b, or zero length
            boost::asio::post(_io_context, [this]() { read_a(); });
            return;
          }

          // Write data to _socket_b
          _socket_b.async_send_to(boost::asio::buffer(_buffer_a, bytes_recvd), remote_b, [this, self](boost::system::error_code ec, std::size_t bytes_sent) {
            // Next Read
            if (ec == boost::asio::error::operation_aborted) {
              // Socket shutdown. Return without scheduling more reads
              return;
            }
            boost::asio::post(_io_context, [this]() { read_a(); });
          });
        });
  }

  void read_b() {
    auto self = shared_from_this();

    boost::asio::ip::udp::endpoint sender_endpoint;

    _socket_b.async_receive_from(
        boost::asio::buffer(_buffer_b), sender_endpoint, [this, self, &sender_endpoint](boost::system::error_code ec, std::size_t bytes_recvd) {
          if (ec == boost::asio::error::operation_aborted) {
            // Socket shutdown. Return without scheduling more reads
            return;
          } else if (ec) {
            _logger->error("Socket B error: " + ec.what());
          }

          // TODO: Implement Queueing
          // Record remote sender
          if (!remote_b_registered) {
            remote_b = sender_endpoint;
            remote_b_registered = true;
            _logger->debug("B side endpoint " + remote_b.address().to_string() + ":" + std::to_string(remote_b.port()));
          }

          // Drop if error, zero length, or other side has not registered
          if (ec || bytes_recvd == 0 || !remote_a_registered) {
            // Ignore errors, packets not from remote_b, or zero length
            boost::asio::post(_io_context, [this]() { read_b(); });
            return;
          }

          // Write data to _socket_a
          _socket_a.async_send_to(boost::asio::buffer(_buffer_b, bytes_recvd), remote_a, [this, self](boost::system::error_code ec, std::size_t bytes_sent) {
            // Next Read
            if (ec == boost::asio::error::operation_aborted) {
              // Socket shutdown. Return without scheduling more reads
              return;
            }
            boost::asio::post(_io_context, [this]() { read_b(); });
          });
        });
  }

 protected:
  std::shared_ptr<Logger> _logger;

  boost::asio::io_context &_io_context;

  boost::asio::ip::udp::socket _socket_a;
  boost::asio::ip::udp::socket _socket_b;

  std::array<char, 65535> _recv_buffer_a{};
  std::array<char, 65535> _recv_buffer_b{};

  boost::asio::mutable_buffer _buffer_a;
  boost::asio::mutable_buffer _buffer_b;
};

}  // namespace athenasip::rtp
