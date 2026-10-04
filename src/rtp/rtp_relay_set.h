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
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

  // One end of the relay. in is what arrived from it, including packets dropped because no other end was known yet; out
  // is what the relay sent to it.
  struct Counts {
    std::uint64_t packets_in = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t packets_out = 0;
    std::uint64_t bytes_out = 0;
  };

  // `relayed` is the owning relay's running total of packets sent on, shared by every set it allocates.
  RTPRelaySet(std::shared_ptr<Logger> logger, const std::string& _bind_address, uint16_t _port, std::shared_ptr<std::atomic<std::uint64_t>> relayed = nullptr)
      : _relayed(std::move(relayed)),
        port(_port),
        bind_address(_bind_address),
        _logger(std::make_shared<LoggerScoped>(_bind_address + ":" + std::to_string(_port), logger)),
        _io_context(detail::get_global_io_context()),
        _socket(_io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(_bind_address), _port)),
        _buffer(boost::asio::buffer(_recv_buffer)) {}

  // The socket belongs to the io_context thread, so the read is started there. dispatch, so a caller already on that
  // thread runs it inline.
  void start() {
    _last_packet_ms.store(_now_ms(), std::memory_order_relaxed);

    auto self = shared_from_this();
    boost::asio::dispatch(_io_context, [this, self]() { read(); });

    _logger->info("Started");
  }

  // Time since a packet last arrived, counted from allocation so a call whose media has not begun reads as young. This
  // is how the node detects a call that ended without a BYE. RTCP is relayed through its own set and counts too, so a
  // held call or a silence-suppressing codec does not look dead (RFC 3550 section 6).
  std::chrono::milliseconds idle_for() const { return std::chrono::milliseconds(_now_ms() - _last_packet_ms.load(std::memory_order_relaxed)); }

  // Where an end said to send its media (RFC 8866 5.7 and 5.14). Each end starts at its described address, so an end
  // that only listens still receives, and moves to wherever its packets really come from once heard (symmetric
  // latching, for NAT). Runs on the io thread, where the endpoints live.
  void expect(const std::string& address, std::uint16_t port) {
    boost::system::error_code ec;
    const auto parsed = boost::asio::ip::make_address(address, ec);
    if (ec || port == 0 || parsed.is_unspecified()) return;

    auto self = shared_from_this();
    auto endpoint = std::make_shared<boost::asio::ip::udp::endpoint>(parsed, port);

    boost::asio::dispatch(_io_context, [this, self, endpoint]() {
      const auto key = _get_endpoint_str(endpoint);
      if (remotes.count(key)) return;

      remotes.emplace(key, endpoint);
      _unheard.insert(key);

      std::lock_guard lock(_counts_mutex);
      if (_slots.emplace(key, _received.size()).second) _received.emplace_back();
    });
  }

  // Per end, in the order the ends became known. Copied under the lock: the io thread counts and any thread may ask.
  std::vector<Counts> counts() const {
    std::lock_guard lock(_counts_mutex);
    return _received;
  }

  // Closes on the io thread and waits, because the port returns to the pool as soon as release_relay_set returns and
  // may be rebound. The wait is bounded because a stopped context never runs the work.
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

  // Every continuation holds self, so a relay released mid-read is not destroyed under it. stop() closes the socket,
  // the receive completes with operation_aborted and the chain ends.
  void read() {
    auto self = shared_from_this();
    auto sender_endpoint = std::make_shared<boost::asio::ip::udp::endpoint>();

    _socket.async_receive_from(
        boost::asio::buffer(_buffer), *sender_endpoint, [this, self, sender_endpoint](boost::system::error_code ec, std::size_t bytes_recvd) {
          if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
              _logger->error("Socket error: " + ec.what());
            }
            return;
          }

          // Any packet arriving, even one dropped below, means the call is live.
          _last_packet_ms.store(_now_ms(), std::memory_order_relaxed);

          auto endpointString = _get_endpoint_str(sender_endpoint);
          auto it = remotes.find(endpointString);

          // First packet from this source. It replaces a described-only end on the same address, or the only end still
          // unheard: a NAT moved the address or port.
          if (it == remotes.end()) {
            const auto replaced = _unheard_for(*sender_endpoint);

            if (!replaced.empty()) {
              _logger->debug("Endpoint " + replaced + " heard from at " + endpointString);
              remotes.erase(replaced);
              _unheard.erase(replaced);

              std::lock_guard lock(_counts_mutex);
              auto slot = _slots.find(replaced);
              if (slot != _slots.end()) {
                _slots.emplace(endpointString, slot->second);
                _slots.erase(slot);
              }
            } else {
              _logger->debug("Registered endpoint " + endpointString);
            }

            remotes.emplace(endpointString, sender_endpoint);
          } else {
            _unheard.erase(endpointString);
          }

          // A bridge has two ends: once two have been heard, ends that were only described are unreachable and are dropped.
          if (!_unheard.empty() && remotes.size() - _unheard.size() >= 2) {
            std::lock_guard lock(_counts_mutex);
            for (const auto& key : _unheard) {
              remotes.erase(key);
              _slots.erase(key);
            }
            _unheard.clear();
          }

          {
            std::lock_guard lock(_counts_mutex);
            auto [slot, added] = _slots.emplace(endpointString, _received.size());
            if (added) _received.emplace_back();
            _received[slot->second].packets_in++;
            _received[slot->second].bytes_in += bytes_recvd;
          }

          if (bytes_recvd == 0) {
            boost::asio::post(_io_context, [this, self]() { read(); });
            return;
          }

          // Nobody to relay to yet.
          if (remotes.size() < 2) {
            boost::asio::post(_io_context, [this, self]() { read(); });
            return;
          }

          // Copied, because the next read reuses the receive buffer before the send completes.
          auto payload = std::make_shared<std::string>(_recv_buffer.data(), bytes_recvd);

          for (const auto& remote : remotes) {
            if (remote.first == endpointString) continue;
            if (_relayed) _relayed->fetch_add(1, std::memory_order_relaxed);
            {
              std::lock_guard lock(_counts_mutex);
              auto slot = _slots.find(remote.first);
              if (slot != _slots.end()) {
                _received[slot->second].packets_out++;
                _received[slot->second].bytes_out += bytes_recvd;
              }
            }
            _socket.async_send_to(boost::asio::buffer(*payload), *(remote.second), [self, payload](boost::system::error_code, std::size_t) {
              // The payload is held until the send completes.
            });
          }

          boost::asio::post(_io_context, [this, self]() { read(); });
        });
  }

 protected:
  std::shared_ptr<std::atomic<std::uint64_t>> _relayed;

  mutable std::mutex _counts_mutex;
  std::unordered_map<std::string, std::size_t> _slots;
  std::vector<Counts> _received;

  std::shared_ptr<Logger> _logger;
  boost::asio::io_context& _io_context;
  boost::asio::ip::udp::socket _socket;
  std::array<char, 65535> _recv_buffer{};
  boost::asio::mutable_buffer _buffer;

  // Relaxed: one store per packet and one load from an infrequent sweep; a stale read is harmless.
  std::atomic<std::int64_t> _last_packet_ms{0};

  static std::int64_t _now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

  // Described ends not yet heard from. Io thread only.
  std::set<std::string> _unheard;

  // The described end a newly heard source stands for: one on the same address, else the only one unheard. Empty for
  // a new end, or when several are unheard and none shares the address.
  std::string _unheard_for(const boost::asio::ip::udp::endpoint& source) const {
    if (_unheard.empty()) return "";

    for (const auto& key : _unheard) {
      const auto found = remotes.find(key);
      if (found != remotes.end() && found->second->address() == source.address()) return key;
    }

    return _unheard.size() == 1 ? *_unheard.begin() : "";
  }

  std::string _get_endpoint_str(std::shared_ptr<boost::asio::ip::udp::endpoint> endpoint) {
    return endpoint->address().to_string() + ":" + std::to_string(endpoint->port());
  }
};

}  // namespace athenasip::rtp
