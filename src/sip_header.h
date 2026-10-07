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

// RFC 3261 7.3.1: field names are case-insensitive, so the lookup map hashes and compares
// them that way. Compact forms are expanded before storage, so "v" and "Via" share a
// bucket.
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

  // True for fields whose grammar is a comma-separated list (RFC 3261 7.3.1). Credential
  // fields are excluded: their commas separate parameters, not values.
  static bool is_list_valued(const std::string& canonical_field_name);

  // Splits a field value on commas that are outside quoted strings and angle brackets.
  static std::vector<std::string> split_field_value(const std::string& value);

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

  // Requests
  std::string request_method;
  std::shared_ptr<SIPUri> request_uri;

  // Responses
  uint16_t response_code = 0;
  std::string response_message;

  std::string sip_version = "SIP/2.0";

  // Every header field, in order, duplicates included.
  std::vector<HeaderField> headers;

  // The same values by field name, case-insensitive, for lookup.
  std::unordered_map<std::string, std::vector<std::shared_ptr<headers::Header>>, FieldNameHash, FieldNameEqual> headers_map;

  void add(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void add(const std::string& field_name, const std::string& value);
  void add_start(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void remove_value(const std::string& field_name, RemoveFn);
  void clear(const std::string& field_name);

  void parse(const std::string& sip_message);
  std::string to_string() const;
  std::string first_line() const;

  // The start line for logging. Unlike first_line(), it never throws on a request whose
  // start line did not parse.
  std::string summary() const;

  bool contains(const std::string& field) const;

  // False when parse() could not read the start line or a header line. An invalid request
  // gets a 400 (RFC 3261 8.2.1, 16.3).
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
