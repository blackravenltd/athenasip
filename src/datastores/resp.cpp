//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "resp.h"

#include <limits>

namespace athenasip::datastores::resp {

std::optional<Reply> Decoder::next() {
  auto at = _position;
  auto reply = _read(at, 0);
  if (!reply) return std::nullopt;

  _position = at;

  // What has been read is let go of, so a long-lived connection's buffer does not grow without end.
  if (_position == _buffer.size()) {
    _buffer.clear();
    _position = 0;
  } else if (_position > 64 * 1024) {
    _buffer.erase(0, _position);
    _position = 0;
  }
  return reply;
}

// One line, up to CRLF, which is not part of it. nullopt while the line is not yet whole; a CR not followed by LF, or
// a line longer than the limit, is refused.
std::optional<std::string_view> Decoder::_line(std::size_t& at) const {
  const auto end = _buffer.find('\r', at);
  if (end == std::string::npos) {
    if (_buffer.size() - at > _limits.max_line) throw ProtocolError("RESP: a line longer than " + std::to_string(_limits.max_line) + " bytes");
    return std::nullopt;
  }
  if (end - at > _limits.max_line) throw ProtocolError("RESP: a line longer than " + std::to_string(_limits.max_line) + " bytes");
  if (end + 1 == _buffer.size()) return std::nullopt;
  if (_buffer[end + 1] != '\n') throw ProtocolError("RESP: a line ended with CR and no LF");

  const std::string_view line(_buffer.data() + at, end - at);
  at = end + 2;
  return line;
}

// A signed decimal integer and nothing else.
std::int64_t Decoder::_number(std::string_view text) {
  if (text.empty()) throw ProtocolError("RESP: an empty number");

  std::size_t i = 0;
  const bool negative = text[0] == '-';
  if (negative) {
    if (text.size() == 1) throw ProtocolError("RESP: a number that is only a sign");
    i = 1;
  }

  std::uint64_t value = 0;
  const std::uint64_t limit = negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1
                                       : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') throw ProtocolError("RESP: not a number: " + std::string(text.substr(0, 32)));
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (limit - digit) / 10) throw ProtocolError("RESP: a number out of range");
    value = value * 10 + digit;
  }

  if (!negative) return static_cast<std::int64_t>(value);
  return value == limit ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(value);
}

std::optional<Reply> Decoder::_read(std::size_t& at, std::size_t depth) const {
  if (at >= _buffer.size()) return std::nullopt;
  if (depth > _limits.max_depth) throw ProtocolError("RESP: arrays nested deeper than " + std::to_string(_limits.max_depth));

  const char type = _buffer[at];
  auto cursor = at + 1;
  const auto line = _line(cursor);
  if (!line) return std::nullopt;

  Reply reply;
  switch (type) {
    case '+':
      reply.type = Reply::Type::SimpleString;
      reply.text = std::string(*line);
      break;

    case '-':
      reply.type = Reply::Type::Error;
      reply.text = std::string(*line);
      break;

    case ':':
      reply.type = Reply::Type::Integer;
      reply.integer = _number(*line);
      break;

    case '$': {
      const auto length = _number(*line);
      if (length == -1) {
        reply.type = Reply::Type::Null;
        break;
      }
      if (length < 0) throw ProtocolError("RESP: a bulk string of negative length");
      if (static_cast<std::uint64_t>(length) > _limits.max_bulk) {
        throw ProtocolError("RESP: a bulk string of " + std::to_string(length) + " bytes, over the limit of " + std::to_string(_limits.max_bulk));
      }

      const auto size = static_cast<std::size_t>(length);
      if (_buffer.size() - cursor < size + 2) return std::nullopt;
      if (_buffer[cursor + size] != '\r' || _buffer[cursor + size + 1] != '\n') throw ProtocolError("RESP: a bulk string longer than its length");

      reply.type = Reply::Type::Bulk;
      reply.text = _buffer.substr(cursor, size);
      cursor += size + 2;
      break;
    }

    case '*': {
      const auto count = _number(*line);
      if (count == -1) {
        reply.type = Reply::Type::Null;
        break;
      }
      if (count < 0) throw ProtocolError("RESP: an array of negative length");
      if (static_cast<std::uint64_t>(count) > _limits.max_elements) {
        throw ProtocolError("RESP: an array of " + std::to_string(count) + " elements, over the limit of " + std::to_string(_limits.max_elements));
      }
      if (depth + 1 > _limits.max_depth) throw ProtocolError("RESP: arrays nested deeper than " + std::to_string(_limits.max_depth));

      reply.type = Reply::Type::Array;
      for (std::int64_t i = 0; i < count; ++i) {
        auto element = _read(cursor, depth + 1);
        if (!element) return std::nullopt;
        reply.elements.push_back(std::move(*element));
      }
      break;
    }

    default:
      throw ProtocolError(std::string("RESP: no such reply type '") + type + "'");
  }

  at = cursor;
  return reply;
}

}  // namespace athenasip::datastores::resp
