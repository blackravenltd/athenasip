//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <vector>

// The cluster's certificate authority, made and used by the node itself so that nobody has
// to learn OpenSSL's command line to run two nodes. Nodes talk SIP to each other over mutual
// TLS, and every node trusts what this CA signed and nothing else.
//
// One directory holds it: ca.key and ca.crt, then <node>.key and <node>.crt for each node.
// Keys are EC P-256 and written readable by their owner only. The CA lasts ten years and a
// node certificate two, renewed by issuing again with replace.
namespace athenasip::ca {

struct Result {
  bool ok = false;
  std::string error;
  std::string certificate;
  std::string key;
};

// A new CA in dir. Refused when one is already there: replacing it would orphan every node
// certificate it signed.
Result init(const std::string& dir);

// A certificate for a node, signed by the CA in dir. It names the node id and every address
// and host name in names, and is good for both ends of a TLS connection. An existing one is
// replaced only when asked.
Result issue_node(const std::string& dir, const std::string& node_id, const std::vector<std::string>& names, bool replace = false);

}  // namespace athenasip::ca
