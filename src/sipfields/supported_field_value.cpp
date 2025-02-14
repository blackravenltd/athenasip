//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "supported_field_value.h"
#include <sstream>
#include <algorithm>
#include <cctype>

using namespace athenasip::sipfields;

// Helper trim function.
// If your project already provides a trim utility, you can use that instead.
static std::string trim(const std::string& s) {
  auto start = s.begin();
  while (start != s.end() && std::isspace(*start)) {
    ++start;
  }
  auto end = s.end();
  do {
    --end;
  } while (std::distance(start, end) > 0 && std::isspace(*end));
  return std::string(start, end + 1);
}

bool SupportedFieldValue::parse(const std::string& val) {
  features.clear();
  std::istringstream iss(val);
  std::string token;
  while (std::getline(iss, token, ',')) {
    std::string trimmed = trim(token);
    if (!trimmed.empty()) {
      features.insert(trimmed);
    }
  }
  return true;
}

std::string SupportedFieldValue::to_string() const {
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
