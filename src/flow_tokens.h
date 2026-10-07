//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <array>
#include <string>

namespace athenasip {

// The flow token in the user part of each Record-Route this node writes (RFC 5626 section
// 5.2), and its mapping back to a flow when it returns in a Route.
//
// The token carries the flow id itself, not a table index: a UDP flow is only an address
// pair (section 3.1) and a call outlives this node's channel entry for it. It is encrypted
// because the flow id is the far end's address and both ends see the Record-Route, and
// authenticated because anyone can write a Route.
//
// The key is random per process, so a token from before a restart or from another node
// does not open, and routing falls back to the Contact.
class FlowTokens {
 public:
  FlowTokens();

  std::string seal(const std::string& flow_id) const;

  // The flow id, or empty if this instance did not seal the token.
  std::string open(const std::string& token) const;

 private:
  std::array<unsigned char, 32> _key{};
};

}  // namespace athenasip
