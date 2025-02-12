//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"

namespace athenasip {

SIPHeader::SIPHeader() {}

SIPHeader::SIPHeader(const std::string& sip_message) { parse(sip_message); }

void SIPHeader::print() const {
  std::cout << "Method: " << method << "\n";
  std::cout << "Request-URI: " << request_uri << "\n";
  std::cout << "SIP Version: " << sip_version << "\nHeaders:\n";
  for (const auto& [key, value] : headers) {
    std::cout << key << ": " << value << "\n";
  }
}

std::string SIPHeader::to_string() const {
  std::string out;

  // Method / URI / Version
  out += method + " " + request_uri + " " + sip_version + "\r\n";

  // Headers
  for (const auto& [key, value] : headers) {
    out += key + ": " + value + "\r\n";
  }

  return out;
}

void SIPHeader::parse(const std::string& sip_message) {
  std::istringstream stream(sip_message);
  std::string line, current_key, current_value;

  // Read the request line (first line)
  if (std::getline(stream, line) && !line.empty()) {
    std::istringstream request_stream(line);
    request_stream >> method >> request_uri >> sip_version;
  }

  while (std::getline(stream, line)) {
    if (line.empty()) break;  // Correctly detect end of headers

    // Handle multi-line header continuation
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      if (!current_key.empty()) {
        current_value += " " + Util::trim(line);
      }
      continue;
    }

    // Store the previous header
    if (!current_key.empty()) {
      headers[current_key] = Util::trim(current_value);
    }

    // Extract new header key and value
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      current_key = line.substr(0, colon_pos);
      current_value = Util::trim(line.substr(colon_pos + 1));
    }
  }

  // Store the last header
  if (!current_key.empty()) {
    headers[current_key] = Util::trim(current_value);
  }
}

std::string operator+(const SIPHeader& header, const std::string& str) { return header.to_string() + str; }
std::string operator+(const std::string& str, const SIPHeader& header) { return str + header.to_string(); }

}  // namespace athenasip
