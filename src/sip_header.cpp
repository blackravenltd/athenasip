//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"
#include <sstream>
#include <iostream>

namespace athenasip {

SIPHeader::SIPHeader(const std::string& sip_message) {
    parse(sip_message);
}

const std::string& SIPHeader::method() const { return _method; }
const std::string& SIPHeader::request_uri() const { return _request_uri; }
const std::string& SIPHeader::sip_version() const { return _sip_version; }
const std::map<std::string, std::string>& SIPHeader::headers() const { return _headers; }

void SIPHeader::print() const {
    std::cout << "Method: " << _method << "\n";
    std::cout << "Request-URI: " << _request_uri << "\n";
    std::cout << "SIP Version: " << _sip_version << "\nHeaders:\n";
    for (const auto& [key, value] : _headers) {
        std::cout << key << ": " << value << "\n";
    }
}

void SIPHeader::parse(const std::string& sip_message) {
    std::istringstream stream(sip_message);
    std::string line;

    // Read the request line (first line)
    if (std::getline(stream, line) && !line.empty()) {
        _request_line = line;
        std::istringstream request_stream(line);
        request_stream >> _method >> _request_uri >> _sip_version;
    }

    std::string current_key, current_value;

    while (std::getline(stream, line)) {
        if (line.empty()) break; // Correctly detect end of headers

        // Handle multi-line header continuation
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!current_key.empty()) {
                current_value += " " + trim(line);
            }
            continue;
        }

        // Store the previous header
        if (!current_key.empty()) {
            _headers[current_key] = trim(current_value);
        }

        // Extract new header key and value
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            current_key = line.substr(0, colon_pos);
            current_value = trim(line.substr(colon_pos + 1));
        }
    }

    // Store the last header
    if (!current_key.empty()) {
        _headers[current_key] = trim(current_value);
    }
}

// Trim leading and trailing whitespace
std::string SIPHeader::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    size_t last = str.find_last_not_of(" \t\r\n");
    return (first == std::string::npos) ? "" : str.substr(first, last - first + 1);
}

} // namespace athenasip
