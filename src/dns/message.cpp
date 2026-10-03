//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/message.h"

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

namespace athenasip::dns {

namespace {

constexpr std::size_t kHeader = 12;
constexpr std::uint16_t kClassIn = 1;

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

// Reads a message front to back. Every read checks its own bounds, so a message that lies
// about its lengths is refused where the lie is, rather than read past.
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& message) : _message(message) {}

  bool u8(std::uint8_t& out) {
    if (_at + 1 > _message.size()) return false;
    out = _message[_at++];
    return true;
  }

  bool u16(std::uint16_t& out) {
    if (_at + 2 > _message.size()) return false;
    out = static_cast<std::uint16_t>((_message[_at] << 8) | _message[_at + 1]);
    _at += 2;
    return true;
  }

  bool u32(std::uint32_t& out) {
    std::uint16_t high = 0;
    std::uint16_t low = 0;
    if (!u16(high) || !u16(low)) return false;
    out = (static_cast<std::uint32_t>(high) << 16) | low;
    return true;
  }

  bool skip(std::size_t count) {
    if (_at + count > _message.size()) return false;
    _at += count;
    return true;
  }

  // RFC 1035 3.3: <character-string> is a length byte and that many bytes.
  bool character_string(std::string& out) {
    std::uint8_t length = 0;
    if (!u8(length) || _at + length > _message.size()) return false;
    out.assign(reinterpret_cast<const char*>(_message.data() + _at), length);
    _at += length;
    return true;
  }

  // RFC 1035 4.1.4: labels, ended by a zero length or by a pointer to where the rest of
  // the name already is. A pointer only ever goes backwards in a name a server wrote, so
  // one that does not is refused, and with it every loop.
  bool name(std::string& out) {
    out.clear();

    std::size_t at = _at;
    bool jumped = false;
    std::size_t after_pointer = 0;

    for (std::size_t guard = 0; guard < 128; ++guard) {
      if (at >= _message.size()) return false;
      const auto length = _message[at];

      if ((length & 0xc0) == 0xc0) {
        if (at + 2 > _message.size()) return false;
        const std::size_t target = (static_cast<std::size_t>(length & 0x3f) << 8) | _message[at + 1];
        if (target >= at) return false;

        if (!jumped) after_pointer = at + 2;
        jumped = true;
        at = target;
        continue;
      }

      // 01 and 10 in the top bits are reserved (RFC 1035 4.1.4, RFC 6891).
      if ((length & 0xc0) != 0) return false;

      if (length == 0) {
        _at = jumped ? after_pointer : at + 1;
        return true;
      }

      if (at + 1 + length > _message.size()) return false;
      if (!out.empty()) out += '.';
      out.append(reinterpret_cast<const char*>(_message.data() + at + 1), length);
      at += 1 + length;
    }

    return false;
  }

  std::size_t at() const { return _at; }
  void seek(std::size_t at) { _at = at; }

 private:
  const std::vector<std::uint8_t>& _message;
  std::size_t _at = 0;
};

bool read_record(Reader& reader, std::optional<Record>& out) {
  Record record;
  std::uint16_t type = 0;
  std::uint16_t klass = 0;
  std::uint16_t length = 0;

  if (!reader.name(record.name) || !reader.u16(type) || !reader.u16(klass) || !reader.u32(record.ttl) || !reader.u16(length)) return false;

  const auto start = reader.at();
  if (!reader.skip(length)) return false;
  const auto end = reader.at();
  reader.seek(start);

  out.reset();

  if (klass == kClassIn) {
    switch (static_cast<Type>(type)) {
      case Type::A: {
        if (length != 4) return false;
        boost::asio::ip::address_v4::bytes_type bytes{};
        for (auto& byte : bytes) reader.u8(byte);
        record.address = boost::asio::ip::address_v4(bytes).to_string();
        record.type = Type::A;
        out = record;
        break;
      }
      case Type::AAAA: {
        if (length != 16) return false;
        boost::asio::ip::address_v6::bytes_type bytes{};
        for (auto& byte : bytes) reader.u8(byte);
        record.address = boost::asio::ip::address_v6(bytes).to_string();
        record.type = Type::AAAA;
        out = record;
        break;
      }
      case Type::SRV:
        if (!reader.u16(record.srv.priority) || !reader.u16(record.srv.weight) || !reader.u16(record.srv.port) || !reader.name(record.srv.target)) return false;
        record.type = Type::SRV;
        out = record;
        break;
      case Type::NAPTR:
        if (!reader.u16(record.naptr.order) || !reader.u16(record.naptr.preference) || !reader.character_string(record.naptr.flags) ||
            !reader.character_string(record.naptr.services) || !reader.character_string(record.naptr.regexp) || !reader.name(record.naptr.replacement)) {
          return false;
        }
        record.type = Type::NAPTR;
        out = record;
        break;
      default:
        break;
    }
  }

  // A record whose contents end before or after the length it declared has been misread,
  // or is lying; either way the rest of the message cannot be trusted to line up.
  if (out && reader.at() != end) return false;

  reader.seek(end);
  return true;
}

}  // namespace

std::vector<std::uint8_t> encode_query(std::uint16_t id, const std::string& name, Type type) {
  std::vector<std::uint8_t> out;

  put16(out, id);
  put16(out, 0x0100);  // QR 0, opcode 0, RD 1
  put16(out, 1);       // QDCOUNT
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);

  std::size_t start = 0;
  while (start < name.size()) {
    auto dot = name.find('.', start);
    if (dot == std::string::npos) dot = name.size();

    const auto label = name.substr(start, dot - start);
    if (!label.empty() && label.size() < 64) {
      out.push_back(static_cast<std::uint8_t>(label.size()));
      out.insert(out.end(), label.begin(), label.end());
    }
    start = dot + 1;
  }
  out.push_back(0);

  put16(out, static_cast<std::uint16_t>(type));
  put16(out, kClassIn);
  return out;
}

std::optional<Response> decode_response(const std::vector<std::uint8_t>& message) {
  if (message.size() < kHeader) return std::nullopt;

  Reader reader(message);
  Response response;

  std::uint16_t flags = 0;
  std::uint16_t questions = 0;
  std::uint16_t answers = 0;
  std::uint16_t authorities = 0;
  std::uint16_t additionals = 0;

  reader.u16(response.id);
  reader.u16(flags);
  reader.u16(questions);
  reader.u16(answers);
  reader.u16(authorities);
  reader.u16(additionals);

  // QR: a query is not an answer to anything.
  if ((flags & 0x8000) == 0) return std::nullopt;

  response.truncated = (flags & 0x0200) != 0;
  response.rcode = static_cast<std::uint8_t>(flags & 0x000f);

  for (std::uint16_t i = 0; i < questions; ++i) {
    std::string name;
    if (!reader.name(name) || !reader.skip(4)) return std::nullopt;
  }

  for (std::uint16_t i = 0; i < answers; ++i) {
    std::optional<Record> record;
    if (!read_record(reader, record)) return std::nullopt;
    if (record) response.answers.push_back(*record);
  }

  // The rest is read to check it is all there, and kept for nothing: a SOA in authority
  // says "no such record", which an empty answer section already says.
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(authorities) + additionals; ++i) {
    std::optional<Record> record;
    if (!read_record(reader, record)) return std::nullopt;
  }

  return response;
}

}  // namespace athenasip::dns
