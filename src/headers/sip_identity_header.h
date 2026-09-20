//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "../types/sip_identity.h"
#include "header.h"

using namespace athenasip;
using namespace athenasip::types;

namespace athenasip::headers {

class SIPIdentityHeader : public Header {
 public:
  SIPIdentityHeader() = default;
  explicit SIPIdentityHeader(const std::string& value) : value(std::make_shared<SIPIdentity>(value)) {}
  explicit SIPIdentityHeader(std::shared_ptr<SIPIdentity> val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  std::shared_ptr<SIPIdentity> value;
};

// Register this field type
struct SIPIdentityHeaderRegister {
  SIPIdentityHeaderRegister() {
    auto reg = []() { return std::make_shared<SIPIdentityHeader>(); };
    Header::register_factory("To", reg);
    Header::register_factory("From", reg);
    Header::register_factory("Contact", reg);

    // RFC 3261 20.30 and 20.34: Route and Record-Route are name-addr with parameters,
    // the same grammar. The proxy has to read the lr parameter of the top Route (16.12)
    // and compare its URI with this node's own, neither of which is possible while the
    // value is a string.
    Header::register_factory("Route", reg);
    Header::register_factory("Record-Route", reg);
  }
};
static SIPIdentityHeaderRegister s_sipIdentityHeaderRegister;

}  // namespace athenasip::headers