//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_identity.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace athenasip::types {

namespace {

std::string to_lower(std::string value) {
  for (auto& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return value;
}

bool is_lws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view value) {
  std::size_t start = 0;
  while (start < value.size() && is_lws(value[start])) ++start;

  std::size_t end = value.size();
  while (end > start && is_lws(value[end - 1])) --end;

  return value.substr(start, end - start);
}

// RFC 3261 25.1 quoted-string. index must be at the opening quote; returns the unescaped contents and moves index
// past the closing quote.
bool read_quoted_string(std::string_view text, std::size_t& index, std::string& out) {
  if (index >= text.size() || text[index] != '"') return false;

  ++index;
  out.clear();

  while (index < text.size()) {
    const char c = text[index];

    if (c == '\\' && index + 1 < text.size()) {
      out.push_back(text[index + 1]);
      index += 2;
      continue;
    }

    if (c == '"') {
      ++index;
      return true;
    }

    out.push_back(c);
    ++index;
  }

  // No closing quote.
  return false;
}

// RFC 3261 25.1 generic-param, with optional whitespace round the '='. A parameter with no '=' has an empty value.
void parse_parameters(std::string_view text, std::unordered_map<std::string, std::string>& out) {
  std::size_t start = 0;

  while (start <= text.size()) {
    auto end = text.find(';', start);
    if (end == std::string_view::npos) end = text.size();

    const auto piece = trim(text.substr(start, end - start));

    if (!piece.empty()) {
      const auto equals = piece.find('=');

      if (equals == std::string_view::npos) {
        out[to_lower(std::string(piece))] = "";
      } else {
        const auto name = trim(piece.substr(0, equals));
        auto value = trim(piece.substr(equals + 1));

        // A quoted value keeps its contents, not its quotes.
        std::string unquoted;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          std::size_t cursor = 0;
          if (read_quoted_string(value, cursor, unquoted)) {
            out[to_lower(std::string(name))] = unquoted;
            if (end == text.size()) break;
            start = end + 1;
            continue;
          }
        }

        out[to_lower(std::string(name))] = std::string(value);
      }
    }

    if (end == text.size()) break;
    start = end + 1;
  }
}

std::string escape_quoted(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);

  for (const char c : value) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }

  return out;
}

}  // namespace

SIPIdentity::SIPIdentity() : wrapped(false), star(false), display_name(std::nullopt), uri(nullptr), tags() {}

SIPIdentity::SIPIdentity(const std::string& identity) : wrapped(false), star(false), display_name(std::nullopt), uri(nullptr), tags() { parse(identity); }

// Hand-written, not a regular expression: this runs on every To, From and Contact from the network, must not
// backtrack, and must handle a quoted display name containing '<' or ';'.
void SIPIdentity::parse(const std::string& identity) {
  wrapped = false;
  star = false;
  display_name.reset();
  uri = nullptr;
  tags.clear();

  const auto text = trim(identity);

  // RFC 3261 20.10: Contact may be "*".
  if (text == "*") {
    star = true;
    return;
  }

  std::size_t cursor = 0;

  // display-name: a quoted-string, or a run of tokens before the '<'.
  if (cursor < text.size() && text[cursor] == '"') {
    std::string name;
    if (read_quoted_string(text, cursor, name)) {
      if (!name.empty()) display_name = name;
    }
  }

  const auto open = text.find('<', cursor);

  if (open != std::string_view::npos) {
    // name-addr. Anything before the '<' not already taken as a quoted display name is an unquoted one.
    if (!display_name.has_value()) {
      const auto candidate = trim(text.substr(cursor, open - cursor));
      if (!candidate.empty()) display_name = std::string(candidate);
    }

    const auto close = text.find('>', open);
    if (close == std::string_view::npos) {
      // No closing bracket: what follows is treated as an addr-spec.
      uri = std::make_shared<SIPUri>(std::string(trim(text.substr(open + 1))));
      return;
    }

    wrapped = true;
    uri = std::make_shared<SIPUri>(std::string(trim(text.substr(open + 1, close - open - 1))));

    // Header parameters follow the '>'.
    const auto after = text.substr(close + 1);
    if (const auto semicolon = after.find(';'); semicolon != std::string_view::npos) {
      parse_parameters(after.substr(semicolon + 1), tags);
    }

    return;
  }

  // addr-spec: with no angle brackets the first semicolon starts the header parameters (RFC 3261 20).
  const auto remainder = text.substr(cursor);
  const auto semicolon = remainder.find(';');

  if (semicolon == std::string_view::npos) {
    uri = std::make_shared<SIPUri>(std::string(trim(remainder)));
    return;
  }

  uri = std::make_shared<SIPUri>(std::string(trim(remainder.substr(0, semicolon))));
  parse_parameters(remainder.substr(semicolon + 1), tags);
}

std::string SIPIdentity::to_string() const {
  if (star) return "*";

  std::ostringstream oss;

  // Always quoted, which is legal for every display name.
  if (display_name.has_value()) {
    oss << "\"" << escape_quoted(*display_name) << "\" ";
  }

  oss << "<" << (uri ? uri->to_string() : std::string{}) << ">";

  // Sorted, so the same identity always produces the same bytes and a retransmission is byte-identical.
  std::vector<const std::pair<const std::string, std::string>*> ordered;
  ordered.reserve(tags.size());
  for (const auto& tag : tags) ordered.push_back(&tag);

  std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });

  for (const auto* tag : ordered) {
    oss << ";" << tag->first;
    if (!tag->second.empty()) oss << "=" << tag->second;
  }

  return oss.str();
}

std::string operator+(const SIPIdentity& identity, const std::string& str) { return identity.to_string() + str; }

std::string operator+(const std::string& str, const SIPIdentity& identity) { return str + identity.to_string(); }

}  // namespace athenasip::types
