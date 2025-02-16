//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "../types/authorization.h"
#include "header.h"

namespace athenasip::headers {

class AuthorizationHeader : public Header {
 public:
  AuthorizationHeader() = default;
  explicit AuthorizationHeader(const std::string& value) : value(std::make_shared<athenasip::types::Authorization>()) { parse(value); }
  explicit AuthorizationHeader(std::shared_ptr<athenasip::types::Authorization> val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public member storing the parsed WWW-Authorization value.
  std::shared_ptr<athenasip::types::Authorization> value;
};

// Register this field type under the "WWW-Authorization" header name.
struct AuthorizationHeaderRegister {
  AuthorizationHeaderRegister() {
    auto reg = []() -> std::shared_ptr<Header> { return std::make_shared<AuthorizationHeader>(); };
    Header::register_factory("WWW-Authorization", reg);
    Header::register_factory("Authorization", reg);
  }
};
static AuthorizationHeaderRegister s_AuthorizationHeaderRegister;

}  // namespace athenasip::headers
