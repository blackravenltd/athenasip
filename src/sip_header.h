//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "util.h"

namespace athenasip {

class SIPHeader : public std::map<std::string, std::string> {
 public:
  SIPHeader();
  SIPHeader(const std::string& sip_message);

  enum Type {
    Request,
    Response,
  } uint8_t;

  Type type = Type::Request;

  // Requests
  std::string request_method;
  std::string request_uri;

  // Responses
  uint16_t response_code;
  std::string response_message;

  // Common
  std::string sip_version = "SIP/2.0";

  void parse(const std::string& sip_message);
  std::string to_string() const;

  bool contains(std::string& field);

  friend std::string operator+(const SIPHeader& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPHeader& header);

  void print() const;

 private:
};

}  // namespace athenasip