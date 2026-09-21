//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

#include "sip_uri.h"

namespace athenasip::types {

// One registered contact for an account: what RFC 3261 section 10 calls a binding.
// An account may have several, and target determination (16.5) uses all of them.
//
// node_id, flow_id and path are empty on a single node. They carry the cluster
// information: which node holds the flow, which flow it is (RFC 5626) and the Path
// header recorded at registration (RFC 3327).
struct Location {
  std::shared_ptr<SIPUri> contact;
  std::uint64_t account_id = 0;

  std::string node_id;
  std::string flow_id;
  std::string path;

  std::time_t registered_at = 0;
  std::time_t expires_at = 0;

  // The contact is on a private address, so it is behind NAT as far as we are
  // concerned and responses have to go back down the flow it arrived on.
  bool nat = false;
};

}  // namespace athenasip::types
