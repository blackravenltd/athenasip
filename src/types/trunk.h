//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace athenasip::types {

// A connection to another SIP service - a carrier, or a PBX this node peers with - that calls leave by and arrive
// from. The node registers to it and answers its challenges; which calls use it is the policy's to say, from the
// trunk's attributes (docs/scripting.md). Kept in the datastore so every node of a cluster has the same trunks.
struct Trunk {
  // Letters, digits, '.', '-' and '_': what a script names it by. Matched without regard to case.
  std::string name;

  // The carrier's domain, which calls and the REGISTER are addressed to: sip:sip.carrier.example;transport=tls.
  // Located by RFC 3263 unless there is a proxy.
  std::string uri;

  // An outbound proxy: where calls and the REGISTER are sent when not where the URI says. Empty for none.
  std::string proxy;

  // The carrier's credentials, for its challenges (RFC 3261 22.2). No username, no answer. The password is kept as
  // given: answering a challenge needs it with the realm the challenge names, which is not known before.
  std::string username;
  std::string password;

  // RFC 3261 10: whether the node registers to the trunk, for how long it asks, and the user part of the Contact
  // it registers, which is where the carrier then sends calls.
  bool register_enabled = false;
  std::uint32_t register_expires = 300;
  std::string contact_user;

  // The addresses a request from this trunk arrives from, as CIDR ranges. A request from one of them is the trunk.
  std::vector<std::string> inbound_addresses;

  // A CA file for TLS to the trunk; empty is the system's store.
  std::string tls_ca;

  // Anything a script wants to know about the trunk: prefixes, numbers, caller id. The node reads none of it.
  boost::json::object attributes;

  std::time_t created_at = 0;

  // The name as stored and looked up.
  std::string key() const { return normalise(name); }
  static std::string normalise(const std::string& name);

  // Whether a name may be stored.
  static bool valid_name(const std::string& name);

  // Whether a text is a CIDR range (192.0.2.0/24, 2001:db8::/32) or a single address.
  static bool valid_range(const std::string& range);

  // Whether an address is in one of the trunk's inbound ranges.
  bool admits(const std::string& address) const;

  // The stored form, password included.
  boost::json::object to_json() const;

  // From the stored form. Throws on what is not a trunk.
  static Trunk from_json(const boost::json::object& stored);
};

// Whether an address is in a CIDR range or is the address given. False for either that does not parse.
bool in_range(const std::string& range, const std::string& address);

}  // namespace athenasip::types
