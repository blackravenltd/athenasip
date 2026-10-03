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

// The flow token this node puts in the user part of each Record-Route it writes (RFC 5626
// section 5.2), and how one coming back in a Route is turned into the flow it names.
//
// It carries the flow id sealed rather than an index into a table. A UDP flow is the pair
// of addresses and nothing more (section 3.1), so it outlives this node forgetting it -
// the idle sweep does that after five quiet minutes, and a call goes on far longer - and
// a token that only named an entry in the channel registry would name nothing by the time
// the BYE came. Sealed, because the flow id is the far end's address and the Record-Route
// goes to both ends of a call this node anchors; authenticated, because the Route it comes
// back in is anybody's to write, and an altered token must not name an address of the
// writer's choosing.
//
// The key is made when the node starts and never leaves it, so a token from before a
// restart, or from another node, opens to nothing - which leaves the Contact, as it did
// before tokens could be opened at all.
class FlowTokens {
 public:
  FlowTokens();

  std::string seal(const std::string& flow_id) const;

  // The flow id, or empty when the token is not one this instance sealed.
  std::string open(const std::string& token) const;

 private:
  std::array<unsigned char, 32> _key{};
};

}  // namespace athenasip
