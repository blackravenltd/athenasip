//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

#include "../util.h"
#include "sip_uri.h"

namespace athenasip::siptypes {

class SIPIdentity {
 public:
  // Constructors
  SIPIdentity();
  explicit SIPIdentity(const std::string& identity);

  bool wrapped;
  std::optional<std::string> display_name;
  std::shared_ptr<SIPUri> uri;

  void parse(const std::string& identity);
  std::string to_string() const;

  friend std::string operator+(const SIPIdentity& identity, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPIdentity& identity);

 private:
  // SIP URI components
};

}  // namespace athenasip::siptypes