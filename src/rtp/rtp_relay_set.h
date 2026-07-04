//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <boost/asio.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

using namespace athenasip::loggers;

namespace athenasip::rtp {

class RTPRelaySet : public std::enable_shared_from_this<RTPRelaySet> {
 public:
  std::string bind_address;
  std::unordered_map<std::string, std::shared_ptr<boost::asio::ip::udp::endpoint>> remotes;
  uint16_t port;

  RTPRelaySet(std::shared_ptr<Logger> logger, const std::string& _bind_address, uint16_t _port)
      : port(_port),
        bind_address(_bind_address),
        _logger(std::make_shared<LoggerScoped>(_bind_address + ":" + std::to_string(_port), logger)),
        _io_context(detail::getGlobalIOContext()),
        _socket(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(_bind_address), _port)),
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
    auto sender_endpoint = std::make_shared<boost::asio::ip::udp::endpoint>();

    _socket.async_receive_from(
        boost::asio::buffer(_buffer), *sender_endpoint, [this, self, sender_endpoint](boost::system::error_code ec, std::size_t bytes_recvd) {
          // Drop packet if error
          if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
              _logger->error("Socket error: " + ec.what());
            }
            return;
          }

          // Create a string key from the endpoint
          auto endpointString = _get_endpoint_str(sender_endpoint);
          // Use unordered_map::find to check if key exists
          auto it = remotes.find(endpointString);

          // Register if first seen
          if (it == remotes.end()) {
            _logger->debug("Registered endpoint " + endpointString);
            remotes.emplace(endpointString, sender_endpoint);
          }

          // Drop packet if zero length
          if (bytes_recvd == 0) {
            boost::asio::post(_io_context, [this]() { read(); });
            return;
          }

          // If less than two remotes are registered, there's no one else to relay to
          if (remotes.size() < 2) {
            boost::asio::post(_io_context, [this]() { read(); });
            return;
          }

          // Relay packet to all other registered endpoints
          for (const auto& remote : remotes) {
            if (remote.first == endpointString) continue;
            _socket.async_send_to(boost::asio::buffer(_buffer, bytes_recvd), *(remote.second), [this, self](boost::system::error_code, std::size_t) {
              // No need to handle send result here
            });
          }

          // Prepare for the next read
          boost::asio::post(_io_context, [this]() { read(); });
        });
  }

 protected:
  std::shared_ptr<Logger> _logger;
  boost::asio::io_context& _io_context;
  boost::asio::ip::udp::socket _socket;
  std::array<char, 65535> _recv_buffer{};
  boost::asio::mutable_buffer _buffer;

  std::string _get_endpoint_str(std::shared_ptr<boost::asio::ip::udp::endpoint> endpoint) {
    return endpoint->address().to_string() + ":" + std::to_string(endpoint->port());
  }
};

}  // namespace athenasip::rtp
