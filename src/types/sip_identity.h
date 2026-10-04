//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "../util.h"
#include "sip_uri.h"

namespace athenasip::types {

// RFC 3261 20.10 and 20.20:
//
//   contact-param = (name-addr / addr-spec) *(SEMI contact-params)
//   name-addr     = [ display-name ] LAQUOT addr-spec RAQUOT
//   display-name  = *(token LWS) / quoted-string
//
// A semicolon inside the angle brackets is a URI parameter; outside them, or anywhere in an addr-spec, it starts
// the header parameters. The two are stored apart.
class SIPIdentity {
 public:
  SIPIdentity();
  explicit SIPIdentity(const std::string& identity);

  bool wrapped;

  // RFC 3261 20.10: Contact may be "*", which with Expires 0 removes every binding (10.2.2). It is not a URI, so it
  // has its own flag and leaves uri null.
  bool star;

  std::optional<std::string> display_name;
  std::shared_ptr<SIPUri> uri;

  // Header parameters. Names are lower-cased on the way in, being case-insensitive (RFC 3261 7.3.1).
  std::unordered_map<std::string, std::string> tags;

  void parse(const std::string& identity);
  std::string to_string() const;

  friend std::string operator+(const SIPIdentity& identity, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPIdentity& identity);
};

}  // namespace athenasip::types
