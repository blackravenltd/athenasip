//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace athenasip::media {

// Bencode as rtpengine's ng protocol uses it: integer, byte string, list or dictionary.
//
// A dictionary is a vector of pairs sorted by key: canonical bencode requires byte-ordered keys, and std::vector is
// the container guaranteed to work with an incomplete type.
//
// The decoder reads untrusted UDP input: recursion is bounded and every length is checked against the input.
class Bencode {
 public:
  enum class Type { Integer, String, List, Dictionary };

  using List = std::vector<Bencode>;
  using Entry = std::pair<std::string, Bencode>;
  using Dictionary = std::vector<Entry>;

  // Defaults to an empty string, which encodes to something valid.
  Bencode() = default;
  explicit Bencode(std::int64_t value);
  explicit Bencode(std::string value);

  static Bencode list(List values);
  static Bencode dictionary(Dictionary entries);

  Type type() const { return _type; }

  bool is_integer() const { return _type == Type::Integer; }
  bool is_string() const { return _type == Type::String; }
  bool is_list() const { return _type == Type::List; }
  bool is_dictionary() const { return _type == Type::Dictionary; }

  // Accessors return an empty value for the wrong type rather than throwing: a mistyped field is treated as missing.
  std::int64_t integer() const { return _type == Type::Integer ? _integer : 0; }
  const std::string& string() const;
  const List& values() const { return _list; }
  const Dictionary& entries() const { return _dictionary; }

  // Dictionary lookup. Null when this is not a dictionary or the key is absent.
  const Bencode* find(const std::string& key) const;
  std::string string_at(const std::string& key) const;
  std::int64_t integer_at(const std::string& key, std::int64_t fallback = 0) const;

  // set() keeps the dictionary sorted and replaces an existing key. Both are no-ops on a value of the wrong type.
  void set(std::string key, Bencode value);
  void append(Bencode value);

  std::string encode() const;

  // One value, followed by nothing but whitespace or padding. std::nullopt for anything malformed, including a length
  // past the end of the input or nesting deeper than kMaxDepth.
  static std::optional<Bencode> decode(std::string_view text);

  // Bounds recursion on hostile input. rtpengine's own replies nest four deep.
  static constexpr unsigned kMaxDepth = 32;

 private:
  static std::optional<Bencode> _decode_value(std::string_view text, std::size_t& at, unsigned depth);
  static std::optional<std::int64_t> _decode_number(std::string_view text, std::size_t& at, char terminator);

  void _encode_into(std::string& out) const;

  Type _type = Type::String;
  std::int64_t _integer = 0;
  std::string _string;
  List _list;
  Dictionary _dictionary;
};

}  // namespace athenasip::media
