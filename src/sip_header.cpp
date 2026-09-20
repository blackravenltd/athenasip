//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"

namespace athenasip {

namespace {

// RFC 3261 20: the canonical long name for every field the RFC defines a compact form
// for, plus the fields we look up by name elsewhere. Keys are lowercase.
const std::unordered_map<std::string, std::string>& canonical_names() {
  static const std::unordered_map<std::string, std::string> names = {
      // Compact forms (RFC 3261 7.3.3, 20)
      {"a", "Accept-Contact"},
      {"b", "Referred-By"},
      {"c", "Content-Type"},
      {"e", "Content-Encoding"},
      {"f", "From"},
      {"i", "Call-ID"},
      {"k", "Supported"},
      {"l", "Content-Length"},
      {"m", "Contact"},
      {"o", "Event"},
      {"r", "Refer-To"},
      {"s", "Subject"},
      {"t", "To"},
      {"u", "Allow-Events"},
      {"v", "Via"},
      {"x", "Session-Expires"},
      // Long forms, so any casing normalises to the spelling used for lookups
      {"accept", "Accept"},
      {"allow", "Allow"},
      {"authorization", "Authorization"},
      {"call-id", "Call-ID"},
      {"contact", "Contact"},
      {"content-encoding", "Content-Encoding"},
      {"content-length", "Content-Length"},
      {"content-type", "Content-Type"},
      {"cseq", "CSeq"},
      {"expires", "Expires"},
      {"from", "From"},
      {"max-forwards", "Max-Forwards"},
      {"min-se", "Min-SE"},
      {"path", "Path"},
      {"proxy-authenticate", "Proxy-Authenticate"},
      {"proxy-authorization", "Proxy-Authorization"},
      {"record-route", "Record-Route"},
      {"require", "Require"},
      {"route", "Route"},
      {"session-expires", "Session-Expires"},
      {"supported", "Supported"},
      {"to", "To"},
      {"via", "Via"},
      {"www-authenticate", "WWW-Authenticate"},
  };
  return names;
}

// RFC 3261 7.3.1: fields whose grammar is 1#(value). The credential fields are not
// here on purpose - their commas separate auth parameters within a single value.
const std::set<std::string>& list_valued_names() {
  static const std::set<std::string> names = {
      "Accept",        "Accept-Encoding",  "Accept-Language",  "Alert-Info", "Allow",       "Call-Info",
      "Contact",       "Content-Encoding", "Content-Language", "Error-Info", "In-Reply-To", "Path",
      "Proxy-Require", "Record-Route",     "Require",          "Route",      "Supported",   "Unsupported",
      "Via",           "Warning",
  };
  return names;
}

std::string to_lower_copy(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

}  // namespace

std::string SIPHeader::canonical_field_name(const std::string& field_name) {
  const auto& names = canonical_names();
  auto it = names.find(to_lower_copy(Util::trim(field_name)));
  if (it != names.end()) return it->second;

  // Unknown field: keep the spelling it arrived with. Lookups are case-insensitive.
  return Util::trim(field_name);
}

bool SIPHeader::is_list_valued(const std::string& canonical_field_name) { return list_valued_names().count(canonical_field_name) > 0; }

std::vector<std::string> SIPHeader::split_field_value(const std::string& value) {
  std::vector<std::string> values;

  bool in_quotes = false;
  int angle_depth = 0;
  std::string current;

  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];

    if (in_quotes) {
      // A backslash inside a quoted string escapes the next character (RFC 3261 25.1).
      if (c == '\\' && i + 1 < value.size()) {
        current += c;
        current += value[++i];
        continue;
      }
      if (c == '"') in_quotes = false;
      current += c;
      continue;
    }

    if (c == '"') {
      in_quotes = true;
      current += c;
      continue;
    }

    if (c == '<') ++angle_depth;
    if (c == '>' && angle_depth > 0) --angle_depth;

    if (c == ',' && angle_depth == 0) {
      values.push_back(Util::trim(current));
      current.clear();
      continue;
    }

    current += c;
  }

  values.push_back(Util::trim(current));

  // An unterminated quote or angle bracket means we cannot trust the split.
  if (in_quotes || angle_depth != 0) return {Util::trim(value)};

  return values;
}

SIPHeader::SIPHeader() {}

SIPHeader::SIPHeader(const std::string& sip_message) { parse(sip_message); }

void SIPHeader::add(const std::string& field_name, std::shared_ptr<headers::Header> value) {
  const auto canonical = canonical_field_name(field_name);
  HeaderField hf{canonical, value};
  headers.push_back(hf);
  headers_map[canonical].push_back(value);
}

void SIPHeader::add(const std::string& field_name, const std::string& value) {
  const auto canonical = canonical_field_name(field_name);

  // A comma-separated list is equivalent to separate rows (RFC 3261 7.3.1).
  if (is_list_valued(canonical)) {
    for (const auto& single : split_field_value(value)) add(canonical, headers::Header::create(canonical, single));
    return;
  }

  add(canonical, headers::Header::create(canonical, value));
}

void SIPHeader::add_start(const std::string& field_name, std::shared_ptr<headers::Header> value) {
  const auto canonical = canonical_field_name(field_name);

  // Create a new HeaderField and push it into the vector.
  HeaderField hf{canonical, value};
  headers.insert(headers.begin(), hf);

  // The map has to prepend too, or headers_map[field][0] is no longer the topmost
  // value and Via lookups pick up the wrong hop.
  auto& vec = headers_map[canonical];
  vec.insert(vec.begin(), value);
}

void SIPHeader::clear(const std::string& field_name) {
  const auto canonical = canonical_field_name(field_name);
  const FieldNameEqual same;

  auto newEnd = std::remove_if(headers.begin(), headers.end(), [&](const auto& item) { return same(item.key, canonical); });
  headers.erase(newEnd, headers.end());
  headers_map.erase(canonical);
}

void SIPHeader::remove_value(const std::string& field_name, std::function<bool(std::shared_ptr<Header> header)> callback) {
  const auto canonical = canonical_field_name(field_name);
  const FieldNameEqual same;

  auto newEnd = std::remove_if(headers.begin(), headers.end(), [&](const auto& item) { return same(item.key, canonical) && callback(item.value); });

  headers.erase(newEnd, headers.end());
  headers_map[canonical].clear();
  for (auto& item : headers) {
    if (same(item.key, canonical)) headers_map[canonical].push_back(item.value);
  }
}

void SIPHeader::_store_parsed_field(const std::string& field_name, const std::string& value) {
  const auto canonical = canonical_field_name(field_name);

  const auto values = is_list_valued(canonical) ? split_field_value(Util::trim(value)) : std::vector<std::string>{Util::trim(value)};

  for (const auto& single : values) {
    auto header = headers::Header::create(canonical, single);
    headers.push_back({canonical, header});
    headers_map[canonical].push_back(header);
  }
}

void SIPHeader::parse(const std::string& sip_message) {
  // Clear any previous state.
  headers.clear();
  headers_map.clear();
  _valid = true;

  std::istringstream stream(sip_message);
  std::string line, current_key, current_value, request_uri_str;

  // Read the request/response line (first line)
  if (std::getline(stream, line) && !Util::trim(line).empty()) {
    std::istringstream first_line_stream(line);
    std::string token;
    first_line_stream >> token;

    if (token == "SIP/2.0") {
      // This is a SIP response. Status-Line is SIP-Version SP Status-Code SP Reason.
      type = Type::Response;
      sip_version = token;  // "SIP/2.0"

      if (!(first_line_stream >> response_code) || response_code < 100 || response_code > 699) _valid = false;

      std::getline(first_line_stream, response_message);
      response_message = Util::trim(response_message);
    } else {
      // This is a SIP request. Request-Line is Method SP Request-URI SP SIP-Version.
      type = Type::Request;
      request_method = token;
      first_line_stream >> request_uri_str >> sip_version;

      if (request_uri_str.empty() || sip_version != "SIP/2.0") {
        _valid = false;
      } else {
        request_uri = std::make_shared<SIPUri>(request_uri_str);
      }
    }
  } else {
    _valid = false;
  }

  // Process header lines
  while (std::getline(stream, line)) {
    // getline splits on LF, so the CR of the CRLF pair is still on the end.
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Stop at an empty line (end of headers)
    if (line.empty()) break;

    // If line begins with space or tab, it's a continuation of the previous header.
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      if (!current_key.empty()) {
        current_value += " " + Util::trim(line);
      }
      continue;
    }

    // If we have a previous header pending, store it.
    if (!current_key.empty()) {
      _store_parsed_field(current_key, current_value);
      current_key.clear();
      current_value.clear();
    }

    // Extract new header key and value. A header line with no colon is malformed.
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      current_key = line.substr(0, colon_pos);
      current_value = Util::trim(line.substr(colon_pos + 1));
    } else {
      _valid = false;
    }
  }

  // Store the last header if present.
  if (!current_key.empty()) _store_parsed_field(current_key, current_value);
}

std::string SIPHeader::to_string() const {
  std::string out = first_line() + "\r\n";

  // Add headers in order.
  for (const auto& header : headers) {
    out += header.key + ": " + header.value->to_string() + "\r\n";
  }

  return out;
}

std::string SIPHeader::first_line() const {
  switch (type) {
    case Type::Request:
      if (!request_uri) throw std::runtime_error("SIPHeader::first_line Request " + request_method + " has no request URI");
      return request_method + " " + request_uri->to_string() + " " + sip_version;
    case Type::Response:
      return sip_version + " " + std::to_string(response_code) + " " + response_message;
    default:
      throw std::runtime_error("SIPHeader::first_line Unknown Type " + std::to_string(type));
  }
}

bool SIPHeader::contains(const std::string& field) const {
  auto it = headers_map.find(canonical_field_name(field));
  return (it != headers_map.end() && !it->second.empty());
}

std::string operator+(const SIPHeader& header, const std::string& str) { return header.to_string() + str; }
std::string operator+(const std::string& str, const SIPHeader& header) { return str + header.to_string(); }

std::string operator+(std::shared_ptr<SIPHeader> header, const std::string& str) { return header->to_string() + str; }
std::string operator+(const std::string& str, std::shared_ptr<SIPHeader> header) { return str + header->to_string(); }

void SIPHeader::print() const {
  switch (type) {
    case Type::Request:
      std::cout << "Request Method: " << request_method << "\n";
      std::cout << "Request URI: " << request_uri << "\n";
      break;
    case Type::Response:
      std::cout << "Response Code: " << response_code << "\n";
      std::cout << "Response Message: " << response_message << "\n";
      break;
    default:
      throw std::runtime_error("SIPHeader::print Unknown Type " + std::to_string(type));
  }
  std::cout << "SIP Version: " << sip_version << std::endl;
  std::cout << "Headers:" << std::endl;

  for (const auto& header : headers) {
    std::cout << header.key << ": " << header.value->to_string() << std::endl;
  }
}

}  // namespace athenasip
