/*
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
*/
#include "sip_message.h"

namespace athenasip {

// Default constructor
SIPMessage::SIPMessage() {}

std::string SIPMessage::to_string() const {
  std::string out;
  out += header->to_string();
  out += "\r\n\r\n";
  out += body;
  return out;
}

void SIPMessage::print() const { std::cout << to_string() << "\r\n"; }

std::string operator+(const SIPMessage& message, const std::string& str) { return message.to_string() + str; }
std::string operator+(const std::string& str, const SIPMessage& message) { return str + message.to_string(); }

}  // namespace athenasip
