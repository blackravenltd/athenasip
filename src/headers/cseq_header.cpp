//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cseq_header.h"

using namespace athenasip::headers;

bool CSeqHeader::parse(const std::string& val) {
  // "171 REGISTER"
  std::istringstream iss(val);

  if (!(iss >> sequence)) {
    return false;
  }

  if (!(iss >> method)) {
    return false;
  }

  // Nothing may follow the method.
  std::string extra;
  if (iss >> extra) {
    return false;
  }

  return true;
}

std::string CSeqHeader::to_string() const { return std::to_string(sequence) + " " + method; }
