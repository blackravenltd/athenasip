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

class RTPRelaySet : public std::enable_shared_from_this<RTPRelaySet> {
 public:
  std::string bind_address;

  std::unordered_set<boost::asio::ip::udp::endpoint> remotes;

  uint16_t port;

  RTPRelaySet(std::shared_ptr<Logger> logger, const std::string &_bind_address, uint16_t _port)
      : _logger(std::make_shared<LoggerScoped>(bind_address + ":" + std::to_string(_port), logger)),
        port(_port),
        bind_address(_bind_address),
        _io_context(detail::getGlobalIOContext()),
        _socket(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(bind_address), _port)),
        _buffer(boost::asio::buffer(_recv_buffer)) {}

  void start() {
    read();
    _logger->info("Started");
  }

  void stop() {
    _socket.close();
    _logger->info("Stopped");
  }

  void read() {
    auto self = shared_from_this();

    boost::asio::ip::udp::endpoint sender_endpoint;

    _socket.async_receive_from(
        boost::asio::buffer(_buffer), sender_endpoint, [this, self, &sender_endpoint](boost::system::error_code ec, std::size_t bytes_recvd) {
          if (ec) {
            if (ec!=boost::asio::error::operation_aborted) {
              _logger->error("Socket error: " + ec.what());
            }
            return;
          }

          // Drop packet if error, zero length, or other side has not registered
          if (bytes_recvd == 0) {
            boost::asio::post(_io_context, [this]() { read(); });
            return;
          }

          // Find the endpoint
          auto it = std::find(remotes.begin(), remotes.end(), sender_endpoint);

          // Register if first seen
          if (it == remotes.end()) {
            _logger->debug("Registered endpoint " + sender_endpoint.address().to_string() + ":" + std::to_string(sender_endpoint.port()));
            remotes.insert(sender_endpoint);
          } 

          // Drop packet if error, zero length, or other side has not registered
          if (remotes.size() < 2) {
            boost::asio::post(_io_context, [this]() { read(); });
            return;
          }

          // Copy to all except sender
          for(const auto& remote : remotes) {
            
            if(remote == sender_endpoint) continue;

            _socket.async_send_to(boost::asio::buffer(_buffer, bytes_recvd), remote, [this, self](boost::system::error_code ec, std::size_t bytes_sent) {
              // We don't care here.
            });
          }

          // Next Read
          boost::asio::post(_io_context, [this]() { read(); });
        });
  }

 protected:
  std::shared_ptr<Logger> _logger;

  boost::asio::io_context &_io_context;

  boost::asio::ip::udp::socket _socket;

  std::array<char, 65535> _recv_buffer{};

  boost::asio::mutable_buffer _buffer;
};

}  // namespace athenasip::rtp
