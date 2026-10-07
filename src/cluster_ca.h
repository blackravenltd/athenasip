//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <vector>

// The cluster's certificate authority, built in so running two nodes needs no OpenSSL
// command line. Nodes talk SIP to each other over mutual TLS and trust only what this CA
// signed.
//
// One directory holds ca.key and ca.crt, then <node>.key and <node>.crt per node. Keys are
// EC P-256, readable by their owner only. The CA lasts ten years and a node certificate
// two; renew by issuing again with replace.
namespace athenasip::ca {

struct Result {
  bool ok = false;
  std::string error;
  std::string certificate;
  std::string key;
};

// Creates a CA in dir. Refuses if one exists: replacing it would orphan every certificate
// it signed.
Result init(const std::string& dir);

// Issues a node certificate signed by the CA in dir, valid for the node id and every
// address or host name in names, as TLS client and server. Overwrites only with replace.
Result issue_node(const std::string& dir, const std::string& node_id, const std::vector<std::string>& names, bool replace = false);

}  // namespace athenasip::ca
