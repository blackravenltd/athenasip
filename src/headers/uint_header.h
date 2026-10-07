//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "header.h"

using namespace athenasip;

namespace athenasip::headers {

class UIntHeader : public Header {
 public:
  UIntHeader() = default;
  explicit UIntHeader(uint64_t val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  uint64_t value = 0;
};

struct UIntHeaderRegister {
  UIntHeaderRegister() {
    auto reg = []() { return std::make_shared<UIntHeader>(); };
    Header::register_factory("Content-Length", reg);
    Header::register_factory("Expires", reg);
    Header::register_factory("Max-Forwards", reg);

    // RFC 3261 20.23: delta-seconds, only on a 423.
    Header::register_factory("Min-Expires", reg);
  }
};
static UIntHeaderRegister s_uintHeaderRegister;

}  // namespace athenasip::headers