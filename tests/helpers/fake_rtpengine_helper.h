//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/bencode.h"

// A UDP socket on localhost that decodes each ng datagram, records it, and answers as the test directs,
// including with an error or not at all.
class FakeRtpengine {
 public:
  FakeRtpengine() : _socket(_io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)) {
    _port = _socket.local_endpoint().port();
    _receive();
    _thread = std::thread([this]() { _io.run(); });
  }

  ~FakeRtpengine() {
    _io.stop();
    if (_thread.joinable()) _thread.join();
  }

  std::uint16_t port() const { return _port; }

  // What to answer a given command with. Anything not named here is answered ok.
  void answer(const std::string& command, athenasip::media::Bencode reply) {
    std::lock_guard<std::mutex> lock(_mutex);
    _replies.insert_or_assign(command, std::move(reply));
  }

  // Ignore the next count datagrams, as if they were lost.
  void swallow(unsigned count) {
    std::lock_guard<std::mutex> lock(_mutex);
    _swallow = count;
  }

  std::vector<athenasip::media::Bencode> requests() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests;
  }

  std::vector<std::string> cookies() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _cookies;
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests.size();
  }

  // The last request for a command.
  std::optional<athenasip::media::Bencode> last(const std::string& command) const {
    std::lock_guard<std::mutex> lock(_mutex);

    for (auto it = _requests.rbegin(); it != _requests.rend(); ++it) {
      if (it->string_at("command") == command) return *it;
    }

    return std::nullopt;
  }

 private:
  void _receive() {
    _socket.async_receive_from(boost::asio::buffer(_buffer), _from, [this](const boost::system::error_code& ec, std::size_t length) {
      if (ec) return;
      _handle(length);
      _receive();
    });
  }

  void _handle(std::size_t length) {
    const std::string datagram(_buffer.data(), length);
    const auto space = datagram.find(' ');
    if (space == std::string::npos) return;

    const auto cookie = datagram.substr(0, space);
    auto request = athenasip::media::Bencode::decode(std::string_view(datagram).substr(space + 1));
    if (!request) return;

    athenasip::media::Bencode reply;

    {
      std::lock_guard<std::mutex> lock(_mutex);

      _cookies.push_back(cookie);
      _requests.push_back(*request);

      if (_swallow > 0) {
        --_swallow;
        return;
      }

      const auto command = request->string_at("command");
      auto canned = _replies.find(command);

      reply = canned != _replies.end() ? canned->second : athenasip::media::Bencode::dictionary({{"result", athenasip::media::Bencode(std::string("ok"))}});
    }

    const auto answer = cookie + " " + reply.encode();

    boost::system::error_code send_ec;
    _socket.send_to(boost::asio::buffer(answer), _from, 0, send_ec);
  }

  boost::asio::io_context _io;
  boost::asio::ip::udp::socket _socket;
  boost::asio::ip::udp::endpoint _from;
  std::array<char, 65535> _buffer{};
  std::uint16_t _port = 0;

  mutable std::mutex _mutex;
  std::map<std::string, athenasip::media::Bencode> _replies;
  std::vector<athenasip::media::Bencode> _requests;
  std::vector<std::string> _cookies;
  unsigned _swallow = 0;

  std::thread _thread;
};
