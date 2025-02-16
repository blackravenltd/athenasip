//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cseq_field_value.h"

#include <sstream>
#include <stdexcept>

using namespace athenasip::sipfields;

bool CSeqFieldValue::parse(const std::string& val) {
  // The expected format is: "171 REGISTER"
  std::istringstream iss(val);

  // Extract the numeric sequence first.
  if (!(iss >> sequence)) {
    return false;
  }

  // Extract the method.
  if (!(iss >> method)) {
    return false;
  }

  // Optionally, ensure there are no extra tokens.
  std::string extra;
  if (iss >> extra) {
    return false;
  }

  return true;
}

std::string CSeqFieldValue::to_string() const { return std::to_string(sequence) + " " + method; }
