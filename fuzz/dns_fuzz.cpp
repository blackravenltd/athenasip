//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// Fuzz target: A DNS response from a resolver.
// The parser may refuse the input by returning or by throwing; it may not crash, hang or read out of bounds.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "dns/message.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string input(reinterpret_cast<const char*>(data), size);
  try {
    (void)athenasip::dns::decode_response(std::vector<std::uint8_t>(data, data + size));
  } catch (const std::exception&) {
  }
  return 0;
}
