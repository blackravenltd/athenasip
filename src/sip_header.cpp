//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"

using namespace athenasip::sipfields;

namespace athenasip {

SIPHeader::SIPHeader() {}

SIPHeader::SIPHeader(const std::string& sip_message) { parse(sip_message); }

void SIPHeader::print() const {
  switch (type) {
    case Type::Request:
      std::cout << "Request Method: " << request_method << "\n";
      std::cout << "Request URI: " << request_uri << "\n";
      break;
    case Type::Response:
      std::cout << "Response Code: " << std::to_string(response_code) << "\n";
      std::cout << "Response Message: " << response_message << "\n";
      break;
    default:
      throw std::runtime_error("SIPHeader::print Unknown Type " + std::to_string(type));
  }
  std::cout << "SIP Version: " << sip_version << "\nHeaders:\n";
  for (const auto& [key, value] : (*this)) {
    std::cout << key << ": " << value << "\n";
  }
}

std::string SIPHeader::to_string() const {
  std::string out;

  // Request/Response
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

  // Headers
  for (const auto& [key, value] : (*this)) {
    out += key + ": " + value->to_string() + "\r\n";
  }

  return out;
}

void SIPHeader::parse(const std::string& sip_message) {
  std::istringstream stream(sip_message);
  std::string line, current_key, current_value;

  // TODO: Fix for responses as well
  // Read the request line (first line)
  if (std::getline(stream, line) && !line.empty()) {
    std::istringstream request_stream(line);
    request_stream >> request_method >> request_uri >> sip_version;
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
      (*this)[current_key] = std::move(FieldValue::create(current_key, Util::trim(current_value)));
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
    (*this)[current_key] = FieldValue::create(current_key, Util::trim(current_value));
  }
}

bool SIPHeader::contains(const std::string field) { return (this->find(field) != this->end()); }

std::string operator+(const SIPHeader& header, const std::string& str) { return header.to_string() + str; }
std::string operator+(const std::string& str, const SIPHeader& header) { return str + header.to_string(); }

}  // namespace athenasip
