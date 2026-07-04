//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "header.h"

namespace athenasip::headers {

class CSeqHeader : public Header {
 public:
  CSeqHeader() = default;
  explicit CSeqHeader(const std::string& value) { parse(value); }
  explicit CSeqHeader(const uint64_t seq, const std::string& val) : sequence(seq), method(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public members to store the CSeq components.
  uint64_t sequence = 0;
  std::string method;
};

// Register this field type under the "CSeq" header name.
struct CSeqHeaderRegister {
  CSeqHeaderRegister() {
    auto reg = []() -> std::shared_ptr<Header> { return std::make_shared<CSeqHeader>(); };
    Header::register_factory("CSeq", reg);
  }
};
static CSeqHeaderRegister s_cseqHeaderRegister;

}  // namespace athenasip::headers
