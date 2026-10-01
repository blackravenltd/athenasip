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

  // One end of the relay, counted at the relay in both directions: in is what arrived from
  // it - including a packet that went nowhere because the other end had not latched yet -
  // and out is what the relay sent to it. One-way audio is one of the two standing still.
  struct Counts {
    std::uint64_t packets_in = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t packets_out = 0;
    std::uint64_t bytes_out = 0;
  };

  // `relayed` is the relay's running total of packets sent on, shared by every set it
  // allocates, so the node can say how much media it has carried over its whole life.
  RTPRelaySet(std::shared_ptr<Logger> logger, const std::string& _bind_address, uint16_t _port, std::shared_ptr<std::atomic<std::uint64_t>> relayed = nullptr)
      : _relayed(std::move(relayed)),
        port(_port),
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

  // Where an end said to send its media (RFC 8866 5.7 and 5.14), before it has sent any.
  // A latching relay that waited to hear from both ends forwarded nothing to one that only
  // listens - muted with silence suppression, an IVR, a recorder - so each end starts at
  // the address its description gave, and moves to wherever its packets really come from
  // once it is heard (symmetric latching, for an end behind a NAT). Handed to the io thread,
  // which is where the endpoints live.
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

  // Per end, in the order the ends were first heard from. Copied out under the lock: the
  // read loop counts on the io thread and this is asked from anywhere.
  std::vector<Counts> counts() const {
    std::lock_guard lock(_counts_mutex);
    return _received;
  }

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

          // Register if first seen. An end that was only described so far is this one if it
          // shares the address, or if it is the only end not heard from yet: that is a NAT
          // having moved the port, or the address, and the described one goes.
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

          // A bridge has two ends. Once two have been heard from, an end that was only ever
          // described is an address nothing answers from - the inside of somebody's NAT -
          // and media stops going there.
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
              // Nothing to do with the result; the payload is held until it is sent.
            });
          }

          // Prepare for the next read
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

  // Relaxed throughout: this is one store on the packet path and one load from a sweep
  // that runs every few minutes. Nothing is ordered against it and a read that is a few
  // microseconds stale cannot change an answer measured in minutes.
  std::atomic<std::int64_t> _last_packet_ms{0};

  static std::int64_t _now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

  // Described ends not yet heard from, on the io thread with the endpoints.
  std::set<std::string> _unheard;

  // The described end a newly heard source stands for: one on the same address, else the
  // only one left unheard. Empty when it is a new end, or when two are unheard and nothing
  // says which.
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
