//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The Redis serialization protocol, version 2, as the redis:// driver speaks it, written by hand in place of
// boost::redis (decided 2026-11-23): a command is an array of bulk strings, and a reply is a simple string, an error,
// an integer, a bulk string, a null or an array of those. The client never sends HELLO 3, so the server never answers
// in RESP3, and the reader refuses RESP3's types rather than guess at them.
namespace athenasip::datastores::resp {

// One reply. An error is a reply, not a failure of the connection: the server answered, and said no.
struct Reply {
  enum class Type { SimpleString, Error, Integer, Bulk, Null, Array };

  Type type = Type::Null;
  std::string text;          // SimpleString, Error, Bulk
  std::int64_t integer = 0;  // Integer
  std::vector<Reply> elements;

  bool is_error() const { return type == Type::Error; }
};

// One or more commands, written as they will go on the wire, so a pipeline is one write.
class Request {
 public:
  template <typename... Args>
  Request& push(std::string_view command, const Args&... args) {
    _payload += "*" + std::to_string(1 + sizeof...(Args)) + "\r\n";
    _bulk(command);
    (_bulk(_text(args)), ...);
    ++_commands;
    return *this;
  }

  template <typename Range>
  Request& push_range(std::string_view command, const Range& args) {
    std::size_t count = 0;
    for (const auto& arg : args) {
      (void)arg;
      ++count;
    }
    _payload += "*" + std::to_string(1 + count) + "\r\n";
    _bulk(command);
    for (const auto& arg : args) _bulk(_text(arg));
    ++_commands;
    return *this;
  }

  // How many replies the request is answered with.
  std::size_t commands() const { return _commands; }
  const std::string& payload() const { return _payload; }

 private:
  template <typename T>
  static std::string _text(const T& value) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
      return std::string(std::string_view(value));
    } else {
      static_assert(std::is_integral_v<T>, "a RESP argument is text or an integer");
      return std::to_string(value);
    }
  }

  void _bulk(std::string_view value) {
    _payload += "$" + std::to_string(value.size()) + "\r\n";
    _payload.append(value);
    _payload += "\r\n";
  }

  std::string _payload;
  std::size_t _commands = 0;
};

// What the reader will hold for one reply, so a server, or whatever is posing as one, cannot make it hold more.
struct Limits {
  std::size_t max_bulk = 64 * 1024 * 1024;  // a record is never more than a few kilobytes
  std::size_t max_elements = 1 << 20;       // SMEMBERS and MGET of a large index
  std::size_t max_depth = 8;                // the driver's replies nest one level
  std::size_t max_line = 64 * 1024;         // a simple string, an error or a length
};

// The bytes are not RESP2, or exceed the limits. The connection is out of step and is dropped.
class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Fed whatever arrived; gives back each reply once it is whole. Throws ProtocolError on bytes it cannot read, after
// which it is of no further use.
class Decoder {
 public:
  explicit Decoder(Limits limits = {}) : _limits(limits) {}

  void feed(std::string_view bytes) { _buffer.append(bytes); }
  std::optional<Reply> next();

  // Bytes held and not yet read into a reply.
  std::size_t pending() const { return _buffer.size() - _position; }

 private:
  // Reads one reply starting at `at`, advancing it past the reply; nullopt when the bytes so far end before it does.
  std::optional<Reply> _read(std::size_t& at, std::size_t depth) const;
  std::optional<std::string_view> _line(std::size_t& at) const;
  static std::int64_t _number(std::string_view text);

  Limits _limits;
  std::string _buffer;
  std::size_t _position = 0;
};

}  // namespace athenasip::datastores::resp
