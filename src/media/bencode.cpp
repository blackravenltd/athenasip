//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "bencode.h"

#include <algorithm>
#include <limits>

namespace athenasip::media {

namespace {

const std::string kEmpty;

bool is_padding(char c) { return c == '\0' || c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

}  // namespace

Bencode::Bencode(std::int64_t value) : _type(Type::Integer), _integer(value) {}

Bencode::Bencode(std::string value) : _type(Type::String), _string(std::move(value)) {}

Bencode Bencode::list(List values) {
  Bencode result;
  result._type = Type::List;
  result._list = std::move(values);
  return result;
}

Bencode Bencode::dictionary(Dictionary entries) {
  Bencode result;
  result._type = Type::Dictionary;
  result._dictionary = std::move(entries);

  std::sort(result._dictionary.begin(), result._dictionary.end(), [](const Entry& a, const Entry& b) { return a.first < b.first; });

  return result;
}

const std::string& Bencode::string() const { return _type == Type::String ? _string : kEmpty; }

const Bencode* Bencode::find(const std::string& key) const {
  if (_type != Type::Dictionary) return nullptr;

  for (const auto& entry : _dictionary) {
    if (entry.first == key) return &entry.second;
  }

  return nullptr;
}

std::string Bencode::string_at(const std::string& key) const {
  const auto* value = find(key);
  return value ? value->string() : std::string();
}

std::int64_t Bencode::integer_at(const std::string& key, std::int64_t fallback) const {
  const auto* value = find(key);
  return (value && value->is_integer()) ? value->integer() : fallback;
}

void Bencode::set(std::string key, Bencode value) {
  if (_type != Type::Dictionary) return;

  for (auto& entry : _dictionary) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return;
    }
  }

  auto at = std::lower_bound(_dictionary.begin(), _dictionary.end(), key, [](const Entry& entry, const std::string& k) { return entry.first < k; });

  _dictionary.insert(at, Entry{std::move(key), std::move(value)});
}

void Bencode::append(Bencode value) {
  if (_type != Type::List) return;
  _list.push_back(std::move(value));
}

std::string Bencode::encode() const {
  std::string out;
  _encode_into(out);
  return out;
}

void Bencode::_encode_into(std::string& out) const {
  switch (_type) {
    case Type::Integer:
      out += 'i';
      out += std::to_string(_integer);
      out += 'e';
      return;

    case Type::String:
      out += std::to_string(_string.size());
      out += ':';
      out += _string;
      return;

    case Type::List:
      out += 'l';
      for (const auto& value : _list) value._encode_into(out);
      out += 'e';
      return;

    case Type::Dictionary:
      // Already in key order: dictionary() sorts and set() inserts in place, which is
      // what canonical bencode asks for and what makes an encoded request comparable
      // byte for byte in a test.
      out += 'd';
      for (const auto& [key, value] : _dictionary) {
        out += std::to_string(key.size());
        out += ':';
        out += key;
        value._encode_into(out);
      }
      out += 'e';
      return;
  }
}

std::optional<Bencode> Bencode::decode(std::string_view text) {
  std::size_t at = 0;

  auto value = _decode_value(text, at, 0);
  if (!value) return std::nullopt;

  // A datagram may be padded; anything else after the value means the two ends do not
  // agree about what was sent, and guessing which part to believe is worse than saying
  // so.
  while (at < text.size() && is_padding(text[at])) ++at;
  if (at != text.size()) return std::nullopt;

  return value;
}

// A bencode integer, and the length prefix of a string, are the same grammar read to a
// different terminator. Leading zeros and "-0" are rejected: the canonical form is the
// only one rtpengine emits, and accepting two spellings of one number in a protocol
// keyed by exact bytes invites a mismatch nobody can see.
std::optional<std::int64_t> Bencode::_decode_number(std::string_view text, std::size_t& at, char terminator) {
  const std::size_t start = at;
  bool negative = false;

  if (at < text.size() && text[at] == '-') {
    negative = true;
    ++at;
  }

  const std::size_t digits_start = at;
  std::int64_t value = 0;

  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    const int digit = text[at] - '0';

    // Bounded before it overflows rather than after: a length field is attacker-facing
    // and signed overflow is undefined, not merely wrong.
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      at = start;
      return std::nullopt;
    }

    value = (value * 10) + digit;
    ++at;
  }

  const std::size_t digit_count = at - digits_start;

  if (digit_count == 0 || at >= text.size() || text[at] != terminator) {
    at = start;
    return std::nullopt;
  }

  if (digit_count > 1 && text[digits_start] == '0') {
    at = start;
    return std::nullopt;
  }

  if (negative && value == 0) {
    at = start;
    return std::nullopt;
  }

  ++at;
  return negative ? -value : value;
}

std::optional<Bencode> Bencode::_decode_value(std::string_view text, std::size_t& at, unsigned depth) {
  if (depth > kMaxDepth || at >= text.size()) return std::nullopt;

  const char lead = text[at];

  if (lead == 'i') {
    ++at;
    auto value = _decode_number(text, at, 'e');
    if (!value) return std::nullopt;
    return Bencode(*value);
  }

  if (lead == 'l') {
    ++at;
    List values;

    while (at < text.size() && text[at] != 'e') {
      auto value = _decode_value(text, at, depth + 1);
      if (!value) return std::nullopt;
      values.push_back(std::move(*value));
    }

    if (at >= text.size()) return std::nullopt;
    ++at;

    return list(std::move(values));
  }

  if (lead == 'd') {
    ++at;
    Dictionary entries;

    while (at < text.size() && text[at] != 'e') {
      auto key = _decode_value(text, at, depth + 1);
      if (!key || !key->is_string()) return std::nullopt;

      auto value = _decode_value(text, at, depth + 1);
      if (!value) return std::nullopt;

      entries.emplace_back(key->string(), std::move(*value));
    }

    if (at >= text.size()) return std::nullopt;
    ++at;

    // Sorted on the way in rather than trusted to arrive that way, so find() answers
    // the same whatever the far end did, and a re-encode is canonical.
    return dictionary(std::move(entries));
  }

  if (lead >= '0' && lead <= '9') {
    auto length = _decode_number(text, at, ':');
    if (!length) return std::nullopt;

    const auto size = static_cast<std::uint64_t>(*length);
    if (size > text.size() - at) return std::nullopt;

    Bencode value{std::string(text.substr(at, static_cast<std::size_t>(size)))};
    at += static_cast<std::size_t>(size);
    return value;
  }

  return std::nullopt;
}

}  // namespace athenasip::media
