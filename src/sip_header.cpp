//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"

namespace athenasip {

SIPHeader::SIPHeader() {}

SIPHeader::SIPHeader(const std::string& sip_message) { parse(sip_message); }

void SIPHeader::add(const std::string& field_name, std::shared_ptr<headers::Header> value) {
  // Create a new HeaderField and push it into the vector.
  HeaderField hf{field_name, value};
  headers.push_back(hf);

  // Update the lookup map.
  headers_map[field_name].push_back(value);
}

void SIPHeader::parse(const std::string& sip_message) {
  // Clear any previous state.
  headers.clear();
  headers_map.clear();

  std::istringstream stream(sip_message);
  std::string line, current_key, current_value;

  // Read the request/response line (first line)
  if (std::getline(stream, line) && !line.empty()) {
    std::istringstream request_stream(line);
    // Assume request line for now (TODO: add response support if needed)
    request_stream >> request_method >> request_uri >> sip_version;
    // In a complete implementation you might set type = Response if the line starts with "SIP/2.0"
  }

  // Process header lines
  while (std::getline(stream, line)) {
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
      auto header = headers::Header::create(current_key, Util::trim(current_value));
      headers.push_back({current_key, header});
      headers_map[current_key].push_back(header);
      current_key.clear();
      current_value.clear();
    }

    // Extract new header key and value.
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      current_key = line.substr(0, colon_pos);
      current_value = Util::trim(line.substr(colon_pos + 1));
    }
  }

  // Store the last header if present.
  if (!current_key.empty()) {
    auto header = headers::Header::create(current_key, Util::trim(current_value));
    headers.push_back({current_key, header});
    headers_map[current_key].push_back(header);
  }
}

std::string SIPHeader::to_string() const {
  std::string out;

  // Build the start-line (request or response)
  switch (type) {
    case Type::Request:
      out += request_method + " " + request_uri + " " + sip_version + "\r\n";
      break;
    case Type::Response:
      out += sip_version + " " + std::to_string(response_code) + " " + response_message + "\r\n";
      break;
    default:
      throw std::runtime_error("SIPHeader::to_string Unknown Type " + std::to_string(type));
  }

  // Add headers in order.
  for (const auto& header : headers) {
    out += header.key + ": " + header.value->to_string() + "\r\n";
  }

  return out;
}

bool SIPHeader::contains(const std::string& field) const {
  auto it = headers_map.find(field);
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
  std::cout << "SIP Version: " << sip_version << "\nHeaders:\n";
  for (const auto& header : headers) {
    std::cout << header.key << ": " << header.value->to_string() << "\n";
  }
}

}  // namespace athenasip
