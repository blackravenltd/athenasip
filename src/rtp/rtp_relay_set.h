//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <future>
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
        _io_context(detail::get_global_io_context()),
        _socket(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(_bind_address), _port)),
        _buffer(boost::asio::buffer(_recv_buffer)) {}

  // A socket belongs to the thread its io_context runs on, and this one runs on the
  // process io_context. Starting a read from whatever thread happened to call in is a
  // second thread inside the socket, which is the rule the transport learned the hard
  // way; the work is handed over instead. dispatch rather than post, so a caller that
  // is already the io thread - which the Core strand is - runs it inline.
  void start() {
    _last_packet_ms.store(_now_ms(), std::memory_order_relaxed);

    auto self = shared_from_this();
    boost::asio::dispatch(_io_context, [this, self]() { read(); });

    _logger->info("Started");
  }

  // When this relay last saw a packet arrive, on the steady clock. It starts at
  // allocation rather than at zero, so a call whose media has not begun yet reads as
  // young rather than as infinitely idle.
  //
  // This is what tells the node a call has gone away without saying so. A phone that
  // loses power sends no BYE, and a proxy holding relay ports for it holds them until it
  // restarts; nothing in the signalling plane will ever say otherwise. RTCP is relayed
  // through a set of its own and counts here too, which is what keeps a call on hold or
  // one whose codec suppresses silence from looking dead (RFC 3550 section 6: reports
  // are sent for the life of the session, whether or not there is anything to carry).
  std::chrono::milliseconds idle_for() const { return std::chrono::milliseconds(_now_ms() - _last_packet_ms.load(std::memory_order_relaxed)); }

  // Closing goes to the io thread for the same reason, and then waits: the port is
  // back in the pool the moment release_relay_set returns and the next allocation may
  // bind it, so the socket has to be shut before then. The wait is bounded because a
  // context that has already stopped will never run the work, and hanging a shutdown
  // on that would be worse than a port that takes a moment longer to come back.
  void stop() {
    auto self = shared_from_this();
    auto closed = std::make_shared<std::promise<void>>();
    auto done = closed->get_future();

    boost::asio::dispatch(_io_context, [this, self, closed]() {
      boost::system::error_code ec;
      _socket.close(ec);
      closed->set_value();
    });

    done.wait_for(std::chrono::seconds(1));
    _logger->info("Stopped");
  }

  // Every continuation carries `self`. The read loop re-arms itself by posting, and a
  // relay released while one of those posts is in flight would otherwise be destroyed
  // under it: the posted lambda held a raw `this` and read() begins with
  // shared_from_this(). stop() closes the socket, so the outstanding receive completes
  // with operation_aborted, the chain ends, and the last reference goes with it.
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

          // Before anything else is decided about it: a packet arrived, so this relay is
          // carrying a live call. Even one that is dropped below says that much.
          _last_packet_ms.store(_now_ms(), std::memory_order_relaxed);

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
            boost::asio::post(_io_context, [this, self]() { read(); });
            return;
          }

          // If less than two remotes are registered, there's no one else to relay to
          if (remotes.size() < 2) {
            boost::asio::post(_io_context, [this, self]() { read(); });
            return;
          }

          // The payload is copied rather than relayed out of the receive buffer. A send
          // holds a reference to its buffer until it completes, and the next read is
          // armed below and will write over that buffer, so relaying from it sends
          // whatever arrived next instead of what was meant.
          auto payload = std::make_shared<std::string>(_recv_buffer.data(), bytes_recvd);

          for (const auto& remote : remotes) {
            if (remote.first == endpointString) continue;
            _socket.async_send_to(boost::asio::buffer(*payload), *(remote.second), [self, payload](boost::system::error_code, std::size_t) {
              // Nothing to do with the result; the payload is held until it is sent.
            });
          }

          // Prepare for the next read
          boost::asio::post(_io_context, [this, self]() { read(); });
        });
  }

 protected:
  std::shared_ptr<Logger> _logger;
  boost::asio::io_context& _io_context;
  boost::asio::ip::udp::socket _socket;
  std::array<char, 65535> _recv_buffer{};
  boost::asio::mutable_buffer _buffer;

  // Relaxed throughout: this is one store on the packet path and one load from a sweep
  // that runs every few minutes. Nothing is ordered against it and a read that is a few
  // microseconds stale cannot change an answer measured in minutes.
  std::atomic<std::int64_t> _last_packet_ms{0};

  static std::int64_t _now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

  std::string _get_endpoint_str(std::shared_ptr<boost::asio::ip::udp::endpoint> endpoint) {
    return endpoint->address().to_string() + ":" + std::to_string(endpoint->port());
  }
};

}  // namespace athenasip::rtp
