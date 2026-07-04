//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <memory>
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

class SIPHeader {
 public:
  using RemoveFn = std::function<bool(std::shared_ptr<Header> header)>;

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

  // Map from header name to vector of values (for quick lookup).
  std::unordered_map<std::string, std::vector<std::shared_ptr<headers::Header>>> headers_map;

  void add(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void add(const std::string& field_name, const std::string& value);
  void add_start(const std::string& field_name, std::shared_ptr<headers::Header> value);
  void remove_value(const std::string& field_name, RemoveFn);
  void clear(const std::string& field_name);

  void parse(const std::string& sip_message);
  std::string to_string() const;
  std::string first_line() const;

  // Returns true if there is at least one header with the given field name.
  bool contains(const std::string& field) const;

  friend std::string operator+(const SIPHeader& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPHeader& header);
  friend std::string operator+(std::shared_ptr<SIPHeader> header, const std::string& str);
  friend std::string operator+(const std::string& str, std::shared_ptr<SIPHeader> header);

  void print() const;
};

}  // namespace athenasip
