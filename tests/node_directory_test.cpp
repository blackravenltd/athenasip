//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "node_directory.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "events/topics.h"
#include "helpers/core_fixture_helper.h"

using namespace athenasip;

namespace {

std::string status(const std::string& node, const std::string& state) {
  return R"({"status":")" + state + R"(","node":")" + node + R"(","version":"1.2.3","datastore":"redis","at":"2026-10-02T10:00:00Z","uptime":5,)" +
         R"("transports":[{"transport":"tls","address":"198.51.100.7","port":5061,"uri":"sips:198.51.100.7:5061;transport=tls"}]})";
}

}  // namespace

// The retained nodes/<id>/status messages are the cluster's list of itself (the 2026-09-21
// decision): each node says what it is and where it listens, and every node that hears it
// knows.
TEST(NodeDirectoryTest, AStatusMessageIsANodeAndWhereItListens) {
  NodeDirectory directory;

  EXPECT_TRUE(directory.observe(events::topics::node_status("node-b"), status("node-b", "ok")));

  const auto nodes = directory.list(std::chrono::seconds(90));
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0].id, "node-b");
  EXPECT_EQ(nodes[0].status, "ok");
  EXPECT_EQ(nodes[0].version, "1.2.3");
  ASSERT_EQ(nodes[0].transports.size(), 1u);
  EXPECT_EQ(nodes[0].transports[0].at("uri").as_string(), "sips:198.51.100.7:5061;transport=tls");
}

// The latest word stands, including the will the broker publishes for a node that
// vanished: a node that is down is listed as down, not dropped, so a client can tell
// "gone" from "never heard of".
TEST(NodeDirectoryTest, TheLatestReportStands) {
  NodeDirectory directory;

  directory.observe(events::topics::node_status("node-b"), status("node-b", "ok"));
  directory.observe(events::topics::node_status("node-b"), status("node-b", "down"));

  const auto nodes = directory.list(std::chrono::seconds(90));
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0].status, "down");
}

// Anything that is not a node's status is not one, and is not guessed at.
TEST(NodeDirectoryTest, WhatIsNotAStatusIsIgnored) {
  NodeDirectory directory;

  EXPECT_FALSE(directory.observe("nodes/node-b/channels/udp/x", status("node-b", "ok")));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), "not json"));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), R"({"node":"node-b"})"));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), status("node-c", "ok"))) << "a message about another node than its topic names";

  EXPECT_TRUE(directory.list(std::chrono::seconds(90)).empty());
}

// A report nobody has repeated for several intervals is stale: the node may be gone in a
// way the broker never noticed, and saying "ok" for it would be the directory lying.
TEST(NodeDirectoryTest, AReportNotRepeatedIsStale) {
  NodeDirectory directory;
  const auto then = std::chrono::steady_clock::now();

  directory.observe(events::topics::node_status("node-b"), status("node-b", "ok"), then);

  EXPECT_FALSE(directory.list(std::chrono::seconds(90), then + std::chrono::seconds(60))[0].stale);
  EXPECT_TRUE(directory.list(std::chrono::seconds(90), then + std::chrono::seconds(91))[0].stale);
}

// And a node hears every node's status, its own included, once its heartbeat starts.
TEST(NodeDirectoryTest, ANodeListensForTheOthers) {
  CoreFixture f;
  f.on_strand([&f]() { f.core->node_status_start(); });
  f.settle();

  f.event_system->publish_state(events::topics::node_status("node-b"), status("node-b", "ok"));
  f.settle();

  const auto nodes = f.core->nodes()->list(std::chrono::seconds(90));
  bool found = false;
  for (const auto& node : nodes) found = found || node.id == "node-b";
  EXPECT_TRUE(found);
}
