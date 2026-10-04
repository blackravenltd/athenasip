//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Example-only code: Asynchronous UDP RTPproxy client with Boost.Asio
// featuring robust error handling, concurrency via strands, etc.
//
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace athenasip::clients {

class RTPProxyResponse {
 public:
  int status_code;  // The first token (e.g., 0 for success, or error code)
  std::string ip;   // If present in response
  int rtp_port;     // If present
  int rtcp_port;    // If present
  bool valid;       // True if parsing was successful

  // Parses an RTPproxy response line, e.g. "0 203.0.113.5 40000 40001".
  explicit RTPProxyResponse(const std::string& resp_line) : status_code(-1), ip(""), rtp_port(-1), rtcp_port(-1), valid(false) { _parse(resp_line); }

  const std::string to_string() const { return std::to_string(status_code) + " " + ip + " " + std::to_string(rtp_port) + " " + std::to_string(rtcp_port); }

 private:
  void _parse(const std::string& resp_line) {
    std::istringstream iss(resp_line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }

    if (tokens.empty()) {
      // No status code: invalid.
      return;
    }

    try {
      status_code = std::stoi(tokens[0]);
    } catch (...) {
      // Not an integer: invalid.
      return;
    }

    // On success (0) the address, RTP port and RTCP port may follow.
    if (status_code == 0 && tokens.size() >= 2) {
      ip = tokens[1];
      if (tokens.size() >= 3) {
        try {
          rtp_port = std::stoi(tokens[2]);
        } catch (...) {
          // An unparseable port stays -1.
        }
      }
      if (tokens.size() >= 4) {
        try {
          rtcp_port = std::stoi(tokens[3]);
        } catch (...) {
          // An unparseable port stays -1.
        }
      }
    }

    valid = true;
  }
};
}  // namespace athenasip::clients
