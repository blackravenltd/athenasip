//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "sip_header.h"
#include "util.h"

namespace athenasip {

class SIPMessage {
 public:
  SIPMessage();

  std::shared_ptr<SIPHeader> header;
  std::string body;
  uint body_length = 0;

  uint16_t source_port;

  std::string to_string() const;

  friend std::string operator+(const SIPMessage& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPMessage& header);

  void print() const;

 private:
};

}  // namespace athenasip