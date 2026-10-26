//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "uint_header.h"

using namespace athenasip;

namespace athenasip::headers {

// RFC 3261 25.1: 1*DIGIT, and nothing else. "12abc", "-1", "" and a value past 64 bits are not numbers; a header
// that fails here is kept as text, and whoever needs the number (Content-Length, Max-Forwards, Expires) sees none.
bool UIntHeader::parse(const std::string& val) {
  std::size_t first = 0;
  std::size_t last = val.size();
  while (first < last && (val[first] == ' ' || val[first] == '\t')) ++first;
  while (last > first && (val[last - 1] == ' ' || val[last - 1] == '\t')) --last;
  if (first == last) return false;

  std::uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(val.data() + first, val.data() + last, parsed);
  if (error != std::errc() || end != val.data() + last) return false;

  value = parsed;
  return true;
}

std::string UIntHeader::to_string() const { return std::to_string(value); }

}  // namespace athenasip::headers