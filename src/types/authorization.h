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
  /// The authentication scheme (e.g. "Digest")
  std::string type;

  /// Public storage for key/value pairs from the header.
  std::unordered_map<std::string, std::string> fields;

  Authorization() = default;

  /// Constructs and immediately parses the given header string.
  /// If parsing fails, the instance will be empty.
  explicit Authorization(const std::string& input) { parse(input); }

  /// Parses the input header string.
  /// Returns true if parsing was successful, false otherwise.
  /// This function does not throw exceptions.
  bool parse(const std::string& input) {
    // Clear any existing content.
    fields.clear();
    type.clear();

    size_t pos = 0;
    const size_t len = input.size();

    // Helper lambda: skip whitespace.
    auto skip_whitespace = [&](size_t& pos) {
      while (pos < len && std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
      }
    };

    // --- Parse the type (e.g. "Digest") ---
    skip_whitespace(pos);
    size_t type_start = pos;
    while (pos < len && !std::isspace(static_cast<unsigned char>(input[pos]))) {
      ++pos;
    }
    if (pos == type_start) {
      return false;  // No type found.
    }
    type = input.substr(type_start, pos - type_start);

    // Skip whitespace after the type.
    skip_whitespace(pos);

    // --- Parse key-value pairs ---
    while (pos < len) {
      // Skip any commas and whitespace.
      while (pos < len && (std::isspace(static_cast<unsigned char>(input[pos])) || input[pos] == ',')) {
        ++pos;
      }
      if (pos >= len) break;

      // Parse key: read until '=' or whitespace.
      size_t key_start = pos;
      while (pos < len && input[pos] != '=' && !std::isspace(static_cast<unsigned char>(input[pos])) && input[pos] != ',') {
        ++pos;
      }
      if (pos == key_start) {
        return false;  // No key found.
      }
      std::string key = Util::trim(input.substr(key_start, pos - key_start));

      // Skip whitespace until the '='.
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
        // Quoted value with escape support.
        ++pos;  // Skip opening quote.
        std::string result;
        while (pos < len) {
          char c = input[pos];
          if (c == '\\') {
            // Escape sequence: include next character literally.
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
        // Unquoted value: read until comma.
        size_t value_start = pos;
        while (pos < len && input[pos] != ',') {
          ++pos;
        }
        value = Util::trim(input.substr(value_start, pos - value_start));
      }
      // Insert the key/value pair.
      fields[key] = value;
    }

    return true;
  }

  bool contains_field(const std::string& field) { return fields.find(field) != fields.end(); }

  /// Converts this header back into a std::string.
  /// The format will be:
  ///     <type> key=value, key="value", ...
  // RFC 7616 section 3 and RFC 3261 25.1: whether a parameter is a token or a quoted
  // string is fixed by the grammar, not by what its value happens to look like.
  // algorithm, stale and nc are tokens, and quoting one is malformed - which is why
  // this server could not send an algorithm at all until now.
  //
  // qop is the awkward one: quoted in a challenge and a token in the credentials a
  // client sends back. This server writes challenges, so it is quoted here; a UAC
  // sending its own credentials will have to say which it is building.
  static bool is_token_parameter(const std::string& name) {
    const auto lowered = Util::to_lower(name);
    return lowered == "algorithm" || lowered == "stale" || lowered == "nc";
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
  // Helper: returns true if the value should be quoted when reconstructing.
  static bool need_quote(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
      // Allow alphanumerics and a few safe punctuation characters.
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == ':')) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace athenasip::types
