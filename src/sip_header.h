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
  SIPHeader(const std::string& sip_message);

  const std::string& method() const;
  const std::string& request_uri() const;
  const std::string& sip_version() const;
  const std::map<std::string, std::string>& headers() const;

  void print() const;
  void parse(const std::string& sip_message);
  std::string to_string() const;

  friend std::string operator+(const SIPHeader& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPHeader& header);

 private:
  std::string _method;
  std::string _request_uri;
  std::string _sip_version;
  std::map<std::string, std::string> _headers;
};

}  // namespace athenasip