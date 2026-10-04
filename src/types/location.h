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

// One registered contact for a subscriber: a binding (RFC 3261 section 10). A subscriber may have several, and
// target determination (16.5) uses all of them.
//
// node_id, flow_id and path are empty on a single node. In a cluster they are the node holding the flow, the flow
// (RFC 5626) and the Path recorded at registration (RFC 3327).
struct Location {
  std::shared_ptr<SIPUri> contact;
  std::uint64_t subscriber_id = 0;

  std::string node_id;
  std::string flow_id;
  std::string path;

  std::time_t registered_at = 0;
  std::time_t expires_at = 0;

  // The contact is on a private address, so it is treated as behind NAT and reached down the flow it registered on.
  bool nat = false;

  // RFC 5626 outbound: the client's +sip.instance and this flow's reg-id, which together identify the binding. Empty
  // and zero for an ordinary binding, which is identified by its contact.
  std::string instance;
  std::uint32_t reg_id = 0;
};

}  // namespace athenasip::types
