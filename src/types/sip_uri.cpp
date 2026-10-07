//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_uri.h"

#include <algorithm>
#include <sstream>

namespace athenasip::types {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equals_ignoring_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;

  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }

  return true;
}

std::string to_lower(std::string_view value) {
  std::string out(value);
  for (auto& c : out) c = lower(c);
  return out;
}

bool is_alphanum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// RFC 3261 25.1: mark = "-" / "_" / "." / "!" / "~" / "*" / "'" / "(" / ")"
// unreserved = alphanum / mark
bool is_unreserved(char c) { return is_alphanum(c) || std::string_view("-_.!~*'()").find(c) != std::string_view::npos; }

// The characters that may appear unescaped in each component (RFC 3261 25.1); any other is percent-escaped. The
// sets differ: a ';' is ordinary in a header value and a delimiter in the parameter list.
bool is_user_char(char c) { return is_unreserved(c) || std::string_view("&=+$,;?/").find(c) != std::string_view::npos; }
bool is_password_char(char c) { return is_unreserved(c) || std::string_view("&=+$,").find(c) != std::string_view::npos; }
bool is_param_char(char c) { return is_unreserved(c) || std::string_view("[]/:&+$").find(c) != std::string_view::npos; }
bool is_header_char(char c) { return is_unreserved(c) || std::string_view("[]/?:+$").find(c) != std::string_view::npos; }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// RFC 3261 19.1.2: escaped = "%" HEX HEX. A '%' not followed by two hex digits is left as it stands.
std::string unescape(std::string_view value) {
  std::string out;
  out.reserve(value.size());

  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int high = hex_value(value[i + 1]);
      const int low = hex_value(value[i + 2]);

      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }

    out.push_back(value[i]);
  }

  return out;
}

template <typename Predicate>
std::string escape(std::string_view value, Predicate allowed) {
  static const char* kHex = "0123456789ABCDEF";

  std::string out;
  out.reserve(value.size());

  for (const char c : value) {
    if (allowed(c)) {
      out.push_back(c);
      continue;
    }

    out.push_back('%');
    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0x0F]);
    out.push_back(kHex[static_cast<unsigned char>(c) & 0x0F]);
  }

  return out;
}

// RFC 3261 19.1.1: port is 1*DIGIT and 16 bits. An overlong run of digits is invalid and must not throw.
bool parse_port(std::string_view text, std::uint16_t& out) {
  if (text.empty() || text.size() > 5) return false;

  unsigned long value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = (value * 10) + static_cast<unsigned long>(c - '0');
  }

  if (value == 0 || value > 65535) return false;

  out = static_cast<std::uint16_t>(value);
  return true;
}

// Splits "a=1;b;c=2" on the separator. A valueless name, such as lr (19.1.1), is kept with an empty value.
SIPUri::Fields split_fields(std::string_view text, char separator) {
  SIPUri::Fields fields;

  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find(separator, start);
    if (end == std::string_view::npos) end = text.size();

    const auto piece = text.substr(start, end - start);
    if (!piece.empty()) {
      const auto equals = piece.find('=');

      if (equals == std::string_view::npos) {
        fields.emplace_back(unescape(piece), std::string{});
      } else {
        fields.emplace_back(unescape(piece.substr(0, equals)), unescape(piece.substr(equals + 1)));
      }
    }

    if (end == text.size()) break;
    start = end + 1;
  }

  return fields;
}

const std::pair<std::string, std::string>* find_field(const SIPUri::Fields& fields, std::string_view name) {
  for (const auto& field : fields) {
    if (equals_ignoring_case(field.first, name)) return &field;
  }

  return nullptr;
}

}  // namespace

SIPUri::SIPUri() : scheme("sip"), user(), password(std::nullopt), host(), port(std::nullopt), valid(false) {}

SIPUri::SIPUri(const std::string& uri) : valid(false) { parse(uri); }

bool SIPUri::has_parameter(std::string_view name) const { return find_field(_parameters, name) != nullptr; }

std::string SIPUri::parameter(std::string_view name) const {
  const auto* field = find_field(_parameters, name);
  return field ? field->second : std::string{};
}

void SIPUri::set_parameter(std::string name, std::string value) {
  for (auto& field : _parameters) {
    if (equals_ignoring_case(field.first, name)) {
      field.second = std::move(value);
      return;
    }
  }

  _parameters.emplace_back(std::move(name), std::move(value));
}

void SIPUri::remove_parameter(std::string_view name) {
  _parameters.erase(std::remove_if(_parameters.begin(), _parameters.end(), [&name](const auto& field) { return equals_ignoring_case(field.first, name); }),
                    _parameters.end());
}

bool SIPUri::has_header(std::string_view name) const { return find_field(_headers, name) != nullptr; }

std::string SIPUri::header(std::string_view name) const {
  const auto* field = find_field(_headers, name);
  return field ? field->second : std::string{};
}

void SIPUri::set_header(std::string name, std::string value) {
  for (auto& field : _headers) {
    if (equals_ignoring_case(field.first, name)) {
      field.second = std::move(value);
      return;
    }
  }

  _headers.emplace_back(std::move(name), std::move(value));
}

void SIPUri::_reset_invalid(const std::string& uri) {
  scheme = "sip";
  user.clear();
  password.reset();
  host = uri;
  port.reset();
  _parameters.clear();
  _headers.clear();
  valid = false;
}

// Hand-written, not a regular expression: the grammar is a sequence of splits on delimiters, and a backtracking
// regex over untrusted text has unbounded recursion.
void SIPUri::parse(const std::string& uri) {
  _parameters.clear();
  _headers.clear();

  const auto scheme_end = uri.find(':');
  if (scheme_end == std::string::npos) {
    return _reset_invalid(uri);
  }

  const auto parsed_scheme = to_lower(std::string_view(uri).substr(0, scheme_end));
  if (parsed_scheme != "sip" && parsed_scheme != "sips") {
    return _reset_invalid(uri);
  }

  std::string_view rest = std::string_view(uri).substr(scheme_end + 1);

  // Headers first: '?' ends the parameter list, and a ';' after it belongs to a header value.
  std::string_view header_text;
  if (const auto question = rest.find('?'); question != std::string_view::npos) {
    header_text = rest.substr(question + 1);
    rest = rest.substr(0, question);
  }

  std::string_view parameter_text;
  if (const auto semicolon = rest.find(';'); semicolon != std::string_view::npos) {
    parameter_text = rest.substr(semicolon + 1);
    rest = rest.substr(0, semicolon);
  }

  // The userinfo ends at the last '@': the host cannot contain one.
  std::string_view host_port = rest;
  if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
    const auto userinfo = rest.substr(0, at);
    host_port = rest.substr(at + 1);

    if (const auto colon = userinfo.find(':'); colon != std::string_view::npos) {
      user = unescape(userinfo.substr(0, colon));
      password = unescape(userinfo.substr(colon + 1));
    } else {
      user = unescape(userinfo);
      password.reset();
    }
  } else {
    user.clear();
    password.reset();
  }

  if (host_port.empty()) {
    return _reset_invalid(uri);
  }

  // An IPv6 reference is bracketed (19.1.1, RFC 5118 section 4), so the port separator is the colon after the ']'.
  std::string_view host_text = host_port;
  std::string_view port_text;

  if (host_port.front() == '[') {
    const auto close = host_port.find(']');
    if (close == std::string_view::npos) {
      return _reset_invalid(uri);
    }

    host_text = host_port.substr(0, close + 1);

    const auto after = host_port.substr(close + 1);
    if (!after.empty()) {
      if (after.front() != ':') {
        return _reset_invalid(uri);
      }
      port_text = after.substr(1);
    }
  } else if (const auto colon = host_port.rfind(':'); colon != std::string_view::npos) {
    host_text = host_port.substr(0, colon);
    port_text = host_port.substr(colon + 1);
  }

  if (host_text.empty()) {
    return _reset_invalid(uri);
  }

  if (!port_text.empty()) {
    std::uint16_t parsed_port = 0;
    if (!parse_port(port_text, parsed_port)) {
      // An unusable port makes the URI invalid.
      return _reset_invalid(uri);
    }
    port = parsed_port;
  } else {
    port.reset();
  }

  scheme = parsed_scheme;
  host = std::string(host_text);
  _parameters = split_fields(parameter_text, ';');
  _headers = split_fields(header_text, '&');
  valid = true;
}

std::string SIPUri::to_string() const {
  std::ostringstream oss;
  oss << scheme << ":";

  if (!user.empty()) {
    oss << escape(user, is_user_char);
    if (password.has_value()) {
      oss << ":" << escape(*password, is_password_char);
    }
    oss << "@";
  }

  oss << host;

  if (port.has_value()) {
    oss << ":" << *port;
  }

  for (const auto& [name, value] : _parameters) {
    oss << ";" << escape(name, is_param_char);
    if (!value.empty()) oss << "=" << escape(value, is_param_char);
  }

  bool first_header = true;
  for (const auto& [name, value] : _headers) {
    oss << (first_header ? '?' : '&') << escape(name, is_header_char) << "=" << escape(value, is_header_char);
    first_header = false;
  }

  return oss.str();
}

// RFC 3261 19.1.4.
bool SIPUri::equivalent_to(const SIPUri& other) const {
  // A SIP and a SIPS URI are never equivalent.
  if (!equals_ignoring_case(scheme, other.scheme)) return false;

  // The user part is case-sensitive; the host is not. A missing port does not equal the default port.
  if (user != other.user) return false;
  if (password.value_or("") != other.password.value_or("")) return false;
  if (!equals_ignoring_case(host, other.host)) return false;
  if (port != other.port) return false;

  // A uri-parameter in both URIs must match; one in only one is ignored, except these four, which change where the
  // request goes.
  static const char* kNeverIgnored[] = {"user", "ttl", "method", "maddr"};

  for (const char* name : kNeverIgnored) {
    const bool mine = has_parameter(name);
    const bool theirs = other.has_parameter(name);

    if (mine != theirs) return false;
    if (mine && !equals_ignoring_case(parameter(name), other.parameter(name))) return false;
  }

  for (const auto& [name, value] : _parameters) {
    if (!other.has_parameter(name)) continue;
    if (!equals_ignoring_case(value, other.parameter(name))) return false;
  }

  // Header components are never ignored: each must be present in both and match.
  if (_headers.size() != other._headers.size()) return false;

  for (const auto& [name, value] : _headers) {
    if (!other.has_header(name)) return false;
    if (value != other.header(name)) return false;
  }

  return true;
}

std::string operator+(const SIPUri& uri, const std::string& str) { return uri.to_string() + str; }

std::string operator+(const std::string& str, const SIPUri& uri) { return str + uri.to_string(); }

}  // namespace athenasip::types
