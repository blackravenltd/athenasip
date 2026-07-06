//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "supported_header.h"

using namespace athenasip::headers;

bool SupportedHeader::parse(const std::string& val) {
  features.clear();
  std::istringstream iss(val);
  std::string token;
  while (std::getline(iss, token, ',')) {
    std::string trimmed = Util::trim(token);
    if (!trimmed.empty()) {
      features.insert(trimmed);
    }
  }
  return true;
}

std::string SupportedHeader::to_string() const {
  std::string result;
  // Note: The order of elements in an unordered_set is unspecified.
  // If order is important, consider storing them in a different container.
  bool first = true;
  for (const auto& feature : features) {
    if (!first) {
      result += ", ";
    }
    result += feature;
    first = false;
  }
  return result;
}
