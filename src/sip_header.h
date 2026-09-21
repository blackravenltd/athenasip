//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "headers/header.h"
#include "headers/string_header.h"
#include "types/sip_uri.h"
#include "util.h"

using namespace athenasip::types;
using namespace athenasip::headers;

namespace athenasip {

// RFC 3261 7.3.1: field names are case-insensitive, so the lookup map compares them
// that way. Compact forms are expanded to their long name before storage instead,
// because "v" and "Via" are the same field and must land in the same bucket.
struct FieldNameHash {
  std::size_t operator()(const std::string& value) const {
    std::size_t hash = 14695981039346656037ULL;
    for (unsigned char c : value) {
      hash ^= static_cast<std::size_t>(std::tolower(c));
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

struct FieldNameEqual {
  bool operator()(const std::string& lhs, const std::string& rhs) const {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) return false;
    }
    return true;
  }
};

class SIPHeader {
 public:
  using RemoveFn = std::function<bool(std::shared_ptr<Header> header)>;

  // Maps a compact form or any casing to the canonical long name (RFC 3261 7.3.3, 20).
  // Unknown fields keep the spelling they arrived with.
  static std::string canonical_field_name(const std::string& field_name);

  // True for fields whose grammar is a comma-separated list (RFC 3261 7.3.1). The
  // credential fields are excluded: their commas separate parameters, not values.
  static bool is_list_valued(const std::string& canonical_field_name);

  // Splits a field value on commas that are outside quoted strings and angle brackets.
  static std::vector<std::string> split_field_value(const std::string& value);

  // A header field holding its key and parsed value.
  struct HeaderField {
    std::string key;
    std::shared_ptr<headers::Header> value;
  };

  SIPHeader();
  explicit SIPHeader(const std::string& sip_message);

  enum Type {
    Request,
    Response,
  };
  Type type = Type::Request;

  // For requests
  std::string request_method;
  std::shared_ptr<SIPUri> request_uri;

  // For responses
  uint16_t response_code = 0;
  std::string response_message;

  // Common SIP version
  std::string sip_version = "SIP/2.0";

  // Primary storage for headers (preserves order and duplicates).
  std::vector<HeaderField> headers;

  // Map from header name to vector of values (for quick lookup). Case-insensitive.
  std::unordered_map<std::string, std::vector<std::shared_ptr<headers::Header>>, FieldNameHash, FieldNameEqual> headers_map;

  void add(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void add(const std::string& field_name, const std::string& value);
  void add_start(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void remove_value(const std::string& field_name, RemoveFn);
  void clear(const std::string& field_name);

  void parse(const std::string& sip_message);
  std::string to_string() const;
  std::string first_line() const;

  // What arrived, for a log line. first_line() is for serialisation and throws on a
  // request whose start line did not parse, and a log line is not allowed to be the
  // thing that kills a node: the read handler that would carry that exception has the
  // process's only stack under it.
  std::string summary() const;

  // Returns true if there is at least one header with the given field name.
  bool contains(const std::string& field) const;

  // False when parse() could not make sense of the start line or a header line. A
  // request that is not valid gets a 400 Bad Request (RFC 3261 8.2.1, 16.3).
  bool is_valid() const { return _valid; }

  friend std::string operator+(const SIPHeader& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPHeader& header);
  friend std::string operator+(std::shared_ptr<SIPHeader> header, const std::string& str);
  friend std::string operator+(const std::string& str, std::shared_ptr<SIPHeader> header);

  void print() const;

 private:
  void _store_parsed_field(const std::string& field_name, const std::string& value);

  bool _valid = true;
};

}  // namespace athenasip
