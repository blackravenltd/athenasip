//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace athenasip {

// The cluster as its nodes describe themselves in the retained nodes/<id>/status messages
// on the event bus. Backs discovery and GET /api/v1/nodes. Not on the call path.
//
// Written from the bus on Core's strand and read by the API on its own executor, hence the
// lock.
class NodeDirectory {
 public:
  struct Node {
    std::string id;
    std::string status;
    std::string version;
    std::string at;
    boost::json::array transports;

    // Where peers reach it for inter-node SIP. Empty and zero for a node outside a cluster,
    // which cannot be forwarded to.
    std::string cluster_address;
    std::uint16_t cluster_port = 0;

    // False when the node has found no peer can reach its inter-node listener: it is not forwarded to.
    bool cluster_reachable = true;

    // The address the node's STUN servers see it at, and the nodes' addresses it has reached with an OPTIONS
    // (AddressDiscovery), as node id and address.
    std::string discovered;
    std::vector<std::pair<std::string, std::string>> reaches;

    // The other nodes' inter-node listeners this node has tried, and whether it got through: node id and result.
    std::vector<std::pair<std::string, bool>> cluster_probes;

    std::chrono::steady_clock::time_point heard{};

    // Set by list() and find(): the last report is older than stale_after.
    bool stale = false;
  };

  // Records a node status message from the bus; returns true if it was one. The topic and
  // the message body must name the same node.
  bool observe(const std::string& topic, const std::string& message, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  // Every node heard from, ordered by id, marked stale when its last report is older than
  // stale_after.
  std::vector<Node> list(std::chrono::seconds stale_after, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

  // One node as it last described itself, or nullopt if never heard from.
  std::optional<Node> find(const std::string& id, std::chrono::seconds stale_after,
                           std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

 private:
  mutable std::mutex _mutex;
  std::map<std::string, Node> _nodes;
};

}  // namespace athenasip
