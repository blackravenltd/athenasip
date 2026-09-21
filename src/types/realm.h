//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::types {

class Realm {
 public:
  // Constructors
  Realm();
  explicit Realm(const std::string& name);

  uint64_t id;
  std::string name;
  std::string nonce_secret;
  uint32_t nonce_expiry = 3600;

  // The longest registration this realm grants. A client asking for more is given this
  // instead (RFC 3261 10.3 step 7, "the registrar MAY choose an expiration less than
  // the requested expiration interval").
  uint32_t registration_timeout = 5000;

  // The shortest registration this realm accepts, and zero - the default - accepts any.
  // A client asking for less is refused with 423 Interval Too Brief and told this value
  // in Min-Expires. The RFC is explicit that this should stay off unless the refreshes
  // are actually costing something: "registrars should accept brief registrations; a
  // request should only be rejected if the interval is so short that the refreshes
  // would degrade registrar performance".
  uint32_t registration_minimum = 0;

  std::string to_string() const;
};

}  // namespace athenasip::types
