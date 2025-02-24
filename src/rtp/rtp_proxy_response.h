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
  // Public fields for direct access
  int status_code;  // The first token (e.g., 0 for success, or error code)
  std::string ip;   // If present in response
  int rtp_port;     // If present
  int rtcp_port;    // If present
  bool valid;       // True if parsing was successful

  /**
   * @brief Constructs and immediately parses the given RTPproxy response line.
   * @param resp_line A line from RTPproxy, e.g. "0 203.0.113.5 40000 40001".
   */
  explicit RTPProxyResponse(const std::string &resp_line) : status_code(-1), ip(""), rtp_port(-1), rtcp_port(-1), valid(false) { _parse(resp_line); }

  const std::string to_string() const { return std::to_string(status_code) + " " + ip + " " + std::to_string(rtp_port) + " " + std::to_string(rtcp_port); }

 private:
  /**
   * @brief Internal parsing method that extracts tokens and populates fields.
   */
  void _parse(const std::string &resp_line) {
    // Split the line by whitespace
    std::istringstream iss(resp_line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }

    // Must have at least one token for status code
    if (tokens.empty()) {
      // No data => invalid
      return;
    }

    // Parse status code
    try {
      status_code = std::stoi(tokens[0]);
    } catch (...) {
      // Not an integer => invalid
      return;
    }

    // If we have a success code (0), possibly more tokens for IP/ports
    // Example: "0 203.0.113.5 40000 40001"
    // - tokens[1] = IP
    // - tokens[2] = RTP port
    // - tokens[3] = RTCP port
    if (status_code == 0 && tokens.size() >= 2) {
      ip = tokens[1];
      if (tokens.size() >= 3) {
        try {
          rtp_port = std::stoi(tokens[2]);
        } catch (...) {
          // If we can't parse the port, we'll leave it as -1
        }
      }
      if (tokens.size() >= 4) {
        try {
          rtcp_port = std::stoi(tokens[3]);
        } catch (...) {
          // If we can't parse the port, we'll leave it as -1
        }
      }
    }

    // If we've reached here, we consider it valid
    valid = true;
  }
};
}  // namespace athenasip::clients
