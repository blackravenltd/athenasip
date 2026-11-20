//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// Fuzz target: A STUN datagram arriving on the SIP UDP port, or a STUN server's answer.
// The parser may refuse the input by returning or by throwing; it may not crash, hang or read out of bounds.

#include <boost/asio/ip/address.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "stun.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string input(reinterpret_cast<const char*>(data), size);
  try {
    (void)athenasip::stun::is_stun(input);
    (void)athenasip::stun::binding_success(input);
    (void)athenasip::stun::binding_response(input, boost::asio::ip::make_address("192.0.2.1"), 5060);
  } catch (const std::exception&) {
  }
  return 0;
}
