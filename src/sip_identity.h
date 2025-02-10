//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <string>

#include "sip_uri.h"
#include "util.h"

namespace athenasip {

class SIPIdentity {
 public:
  // Constructors
  SIPIdentity();
  explicit SIPIdentity(const std::string& identity);

  // Accessors
  std::optional<std::string> display_name() const;
  const SIPUri& uri() const;

  std::string to_string() const;

  // Mutators
  void set_display_name(const std::string& name);
  void set_uri(const SIPUri& uri);
  void clear_display_name();

  friend std::string operator+(const SIPIdentity& identity, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPIdentity& identity);

 private:
  void parse(const std::string& identity);

  // SIP URI components
  std::optional<std::string> _display_name;
  SIPUri _uri;
};

}  // namespace athenasip