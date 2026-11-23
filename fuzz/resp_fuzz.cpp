//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// Fuzz target: Redis replies, read from a TCP or TLS connection, fed whole and then split at a point the input chooses.
// The reader may refuse the input by throwing; it may not crash, hang or read out of bounds.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "datastores/resp.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using athenasip::datastores::resp::Decoder;
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  try {
    Decoder whole;
    whole.feed(input);
    while (whole.next()) {
    }
  } catch (const std::exception&) {
  }

  try {
    const auto split = size == 0 ? 0 : data[0] % size;
    Decoder parts;
    parts.feed(input.substr(0, split));
    while (parts.next()) {
    }
    parts.feed(input.substr(split));
    while (parts.next()) {
    }
  } catch (const std::exception&) {
  }
  return 0;
}
