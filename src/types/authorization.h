//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cctype>
#include <string>
#include <unordered_map>

#include "../util.h"

namespace athenasip::types {

class Authorization {
 public:
  // The authentication scheme, e.g. "Digest".
  std::string type;

  // The parameters.
  std::unordered_map<std::string, std::string> fields;

  Authorization() = default;

  // Parses the given header value; empty if it does not parse.
  explicit Authorization(const std::string& input) { parse(input); }

  // False if the value does not parse. Does not throw.
  bool parse(const std::string& input) {
    fields.clear();
    type.clear();

    size_t pos = 0;
    const size_t len = input.size();

    auto skip_whitespace = [&](size_t& pos) {
      while (pos < len && std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
      }
    };

    // The scheme.
    skip_whitespace(pos);
    size_t type_start = pos;
    while (pos < len && !std::isspace(static_cast<unsigned char>(input[pos]))) {
      ++pos;
    }
    if (pos == type_start) {
      return false;  // No type found.
    }
    type = input.substr(type_start, pos - type_start);

    skip_whitespace(pos);

    // The parameters: key=value, comma-separated.
    while (pos < len) {
      while (pos < len && (std::isspace(static_cast<unsigned char>(input[pos])) || input[pos] == ',')) {
        ++pos;
      }
      if (pos >= len) break;

      size_t key_start = pos;
      while (pos < len && input[pos] != '=' && !std::isspace(static_cast<unsigned char>(input[pos])) && input[pos] != ',') {
        ++pos;
      }
      if (pos == key_start) {
        return false;  // No key found.
      }
      std::string key = Util::trim(input.substr(key_start, pos - key_start));

      skip_whitespace(pos);
      if (pos >= len || input[pos] != '=') {
        return false;  // Expected '=' after key.
      }
      ++pos;  // Skip '='
      skip_whitespace(pos);
      if (pos >= len) {
        return false;  // Expected value after '='.
      }

      std::string value;
      if (input[pos] == '"') {
        // Quoted, with backslash escapes.
        ++pos;  // Skip opening quote.
        std::string result;
        while (pos < len) {
          char c = input[pos];
          if (c == '\\') {
            if (pos + 1 < len) {
              ++pos;
              result.push_back(input[pos]);
            } else {
              return false;  // Invalid escape at end of input.
            }
          } else if (c == '"') {
            break;  // Closing quote found.
          } else {
            result.push_back(c);
          }
          ++pos;
        }
        if (pos >= len || input[pos] != '"') {
          return false;  // Unterminated quoted value.
        }
        value = result;
        ++pos;  // Skip closing quote.
      } else {
        // Unquoted: up to the next comma.
        size_t value_start = pos;
        while (pos < len && input[pos] != ',') {
          ++pos;
        }
        value = Util::trim(input.substr(value_start, pos - value_start));
      }
      fields[key] = value;
    }

    return true;
  }

  bool contains_field(const std::string& field) { return fields.find(field) != fields.end(); }

  // RFC 7616 section 3 and RFC 3261 25.1: the grammar fixes whether a parameter is a token or a quoted string.
  // algorithm, stale and nc are tokens, and quoting one is malformed. qop is quoted in a challenge and a token in
  // credentials; this server writes challenges, so it is quoted here.
  static bool is_token_parameter(const std::string& name) {
    const auto lowered = Util::to_lower(name);
    // qop is a token in credentials (RFC 3261 25.1 message-qop, RFC 7616 3.4); only a challenge's qop-options is
    // quoted, and this node writes no qop into a challenge.
    return lowered == "algorithm" || lowered == "stale" || lowered == "nc" || lowered == "qop";
  }

  std::string to_string() const {
    std::string result = type;
    bool first = true;
    for (const auto& kv : fields) {
      if (first) {
        result += " ";
        first = false;
      } else {
        result += ", ";
      }

      if (is_token_parameter(kv.first)) {
        result += kv.first + "=" + kv.second;
        continue;
      }

      result += kv.first + "=\"" + kv.second + "\"";
    }
    return result;
  }

 private:
  // Whether a value must be quoted when written.
  static bool need_quote(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == ':')) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace athenasip::types
