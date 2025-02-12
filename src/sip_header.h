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

class SIPHeader {
 public:
  SIPHeader();
  SIPHeader(const std::string& sip_message);

  std::string method;
  std::string request_uri;
  std::string sip_version;
  std::map<std::string, std::string> headers;

  void parse(const std::string& sip_message);
  std::string to_string() const;

  friend std::string operator+(const SIPHeader& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPHeader& header);

  void print() const;

 private:
};

}  // namespace athenasip