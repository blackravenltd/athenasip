//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "header.h"

namespace athenasip::headers {

// RFC 4028 section 4:
//
//   Session-Expires  = ("Session-Expires" / "x") HCOLON delta-seconds *(SEMI se-params)
//   se-params        = refresher-param / generic-param
//   refresher-param  = "refresher" EQUAL ("uas" / "uac")
//   Min-SE           = "Min-SE" HCOLON delta-seconds *(SEMI generic-param)
//
// One type for both. Min-SE is the same delta-seconds with no refresher, and a field
// that cannot carry one simply leaves it empty; giving it a type of its own would buy
// nothing but a second parser to keep in step.
class SessionExpiresHeader : public Header {
 public:
  SessionExpiresHeader() = default;
  explicit SessionExpiresHeader(std::uint32_t seconds) : delta_seconds(seconds) {}
  explicit SessionExpiresHeader(std::uint32_t seconds, std::string who) : delta_seconds(seconds), refresher(std::move(who)) {}
  explicit SessionExpiresHeader(const std::string& value) { parse(value); }

  bool parse(const std::string& value) override;
  std::string to_string() const override;

  std::uint32_t delta_seconds = 0;

  // "uac" or "uas", lower-cased (7.3.1 makes parameter names case-insensitive and 4028
  // gives the value only these two spellings). Empty when the field did not say, which
  // RFC 4028 section 7.1 allows and leaves for the endpoints to settle.
  std::string refresher;
};

// Register this field type
struct SessionExpiresHeaderRegister {
  SessionExpiresHeaderRegister() {
    auto reg = []() { return std::make_shared<SessionExpiresHeader>(); };
    Header::register_factory("Session-Expires", reg);
    Header::register_factory("Min-SE", reg);
  }
};
static SessionExpiresHeaderRegister s_sessionExpiresHeaderRegister;

}  // namespace athenasip::headers
