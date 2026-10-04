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

// The retained nodes/<id>/status messages are the cluster's directory: each says what a node is and
// where it listens.
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

// The latest report stands, including the broker's will for a vanished node: it is listed as down,
// not dropped.
TEST(NodeDirectoryTest, TheLatestReportStands) {
  NodeDirectory directory;

  directory.observe(events::topics::node_status("node-b"), status("node-b", "ok"));
  directory.observe(events::topics::node_status("node-b"), status("node-b", "down"));

  const auto nodes = directory.list(std::chrono::seconds(90));
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes[0].status, "down");
}

// Messages that are not a node status are ignored.
TEST(NodeDirectoryTest, WhatIsNotAStatusIsIgnored) {
  NodeDirectory directory;

  EXPECT_FALSE(directory.observe("nodes/node-b/channels/udp/x", status("node-b", "ok")));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), "not json"));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), R"({"node":"node-b"})"));
  EXPECT_FALSE(directory.observe(events::topics::node_status("node-b"), status("node-c", "ok"))) << "a message about another node than its topic names";

  EXPECT_TRUE(directory.list(std::chrono::seconds(90)).empty());
}

// A report not repeated for several intervals is stale: the node may have gone without the broker noticing.
TEST(NodeDirectoryTest, AReportNotRepeatedIsStale) {
  NodeDirectory directory;
  const auto then = std::chrono::steady_clock::now();

  directory.observe(events::topics::node_status("node-b"), status("node-b", "ok"), then);

  EXPECT_FALSE(directory.list(std::chrono::seconds(90), then + std::chrono::seconds(60))[0].stale);
  EXPECT_TRUE(directory.list(std::chrono::seconds(90), then + std::chrono::seconds(91))[0].stale);
}

// Once its heartbeat starts, a node hears every node's status, its own included.
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

// A status carries the node's mutual-TLS cluster listener, where peers forward requests for flows it holds.
TEST(NodeDirectoryTest, ANodeSaysWhereItsPeersReachIt) {
  NodeDirectory directory;

  const std::string with_cluster = R"({"status":"ok","node":"node-b","version":"1.2.3","at":"2026-10-03T10:00:00Z","transports":[],)"
                                   R"("cluster":{"address":"10.0.0.2","port":5062}})";
  ASSERT_TRUE(directory.observe(events::topics::node_status("node-b"), with_cluster));
  ASSERT_TRUE(directory.observe(events::topics::node_status("node-c"), status("node-c", "ok")));

  const auto b = directory.find("node-b", std::chrono::seconds(90));
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->cluster_address, "10.0.0.2");
  EXPECT_EQ(b->cluster_port, 5062);

  // A node outside a cluster advertises no listener and is not a forwarding target.
  const auto c = directory.find("node-c", std::chrono::seconds(90));
  ASSERT_TRUE(c.has_value());
  EXPECT_TRUE(c->cluster_address.empty());
  EXPECT_EQ(c->cluster_port, 0);

  EXPECT_FALSE(directory.find("node-z", std::chrono::seconds(90)).has_value());
}
