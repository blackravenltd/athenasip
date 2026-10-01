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
#include <string>
#include <vector>

namespace athenasip {

// The cluster as its nodes describe themselves: the retained nodes/<id>/status messages on
// the event bus, each saying what a node is, how it is and where it listens. It is the
// discovery the 2026-09-21 decision gave the bus, and what GET /api/v1/nodes lists, so a
// client asking any node learns the same cluster.
//
// Written from the bus on Core's strand and read by the API on its own executor, hence the
// lock. Nothing here is on the call path.
class NodeDirectory {
 public:
  struct Node {
    std::string id;
    std::string status;
    std::string version;
    std::string at;
    boost::json::array transports;

    std::chrono::steady_clock::time_point heard{};

    // Set by list(): not repeated within the window, so not to be believed.
    bool stale = false;
  };

  // A message from the bus. True when it was a node's status and was taken. The topic and
  // the message have to name the same node: a status published under another node's topic
  // is not one.
  bool observe(const std::string& topic, const std::string& message, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  // Every node heard from, ordered by id, marked stale when its last report is older than
  // stale_after.
  std::vector<Node> list(std::chrono::seconds stale_after, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

 private:
  mutable std::mutex _mutex;
  std::map<std::string, Node> _nodes;
};

}  // namespace athenasip
