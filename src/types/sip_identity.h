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
// The distinction that matters is where a semicolon belongs. Inside the angle brackets
// it is a URI parameter; outside them, or anywhere in an addr-spec, it starts the
// header parameters. They mean different things and are stored apart.
class SIPIdentity {
 public:
  // Constructors
  SIPIdentity();
  explicit SIPIdentity(const std::string& identity);

  // Data members
  bool wrapped;

  // RFC 3261 20.10: Contact has "*" as an alternative to a contact-param, and 10.2.2
  // gives it its meaning - with Expires 0 it removes every binding. It is the grammar's
  // own alternative rather than a URI, so it gets its own flag and leaves uri null.
  // Before this it was parsed as a URI and recognised by the shape of the wreckage.
  bool star;

  std::optional<std::string> display_name;
  std::shared_ptr<SIPUri> uri;

  // Header parameters. Names are lower-cased on the way in because RFC 3261 7.3.1 makes
  // them case-insensitive, and every lookup here spells them in lower case.
  std::unordered_map<std::string, std::string> tags;

  // Member functions
  void parse(const std::string& identity);
  std::string to_string() const;

  // Friend concatenation operators
  friend std::string operator+(const SIPIdentity& identity, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPIdentity& identity);
};

}  // namespace athenasip::types
