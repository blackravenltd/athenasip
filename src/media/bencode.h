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

// Bencode, as rtpengine's ng protocol carries it: an integer, a byte string, a list or
// a dictionary, and nothing else. Hand-written rather than pulled in, per the project's
// dependency rule, and small enough that the rule costs nothing here.
//
// A dictionary is a vector of pairs kept sorted by key rather than a std::map, for two
// reasons. Bencode's canonical form requires keys in byte order, so sorted is what has
// to be emitted anyway; and a container of an incomplete type is only guaranteed to
// work for std::vector.
//
// The decoder reads data off a UDP socket, so it is written the way the SIP parsers
// are: no recursion that the input can drive without bound, and every length checked
// against what is actually there before it is trusted.
class Bencode {
 public:
  enum class Type { Integer, String, List, Dictionary };

  using List = std::vector<Bencode>;
  using Entry = std::pair<std::string, Bencode>;
  using Dictionary = std::vector<Entry>;

  // The neutral value is an empty string, so a default-constructed Bencode encodes to
  // something valid rather than to nothing.
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

  // Accessors that answer for the wrong type rather than throwing. A field that is not
  // what the protocol said it would be is the same problem as a field that is missing,
  // and every caller here treats it that way.
  std::int64_t integer() const { return _type == Type::Integer ? _integer : 0; }
  const std::string& string() const;
  const List& values() const { return _list; }
  const Dictionary& entries() const { return _dictionary; }

  // Dictionary lookup. Null when this is not a dictionary or the key is absent.
  const Bencode* find(const std::string& key) const;
  std::string string_at(const std::string& key) const;
  std::int64_t integer_at(const std::string& key, std::int64_t fallback = 0) const;

  // Building. set() keeps the dictionary sorted and replaces a key already there;
  // both are no-ops on a value of the wrong type.
  void set(std::string key, Bencode value);
  void append(Bencode value);

  std::string encode() const;

  // One value, with nothing but whitespace or padding after it. std::nullopt for
  // anything malformed, which includes a length that runs past the end of the input
  // and a structure nested deeper than kMaxDepth.
  static std::optional<Bencode> decode(std::string_view text);

  // Deep enough for anything the ng protocol sends and shallow enough that a hostile
  // datagram cannot walk the stack off the end. rtpengine's own replies reach four.
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
