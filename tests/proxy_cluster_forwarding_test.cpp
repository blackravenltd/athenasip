//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "events/topics.h"
#include "helpers/proxy_fixture_helper.h"
#include "node_directory.h"
#include "types/location.h"

using namespace athenasip;

namespace {

// Two nodes and one datastore, seen from node "test-node". Bindings are shared and flows are not: a
// node that reads a binding whose flow another node holds forwards to that node, which delivers.
struct ClusterFixture : ProxyFixture {
  std::shared_ptr<MockConnection> peer_connection;
  std::shared_ptr<Channel> peer;

  ClusterFixture() {
    // node-b's status as published on the bus, and the mutual-TLS flow to it.
    hear("node-b", "ok", "198.51.100.80");
    peer = make_channel("198.51.100.80", &peer_connection, "tls", 5062);
    peer_connection->peer = "node-b";
  }

  void hear(const std::string& node, const std::string& state, const std::string& address) {
    const auto report = R"({"status":")" + state + R"(","node":")" + node + R"(","version":"1.0.0","at":"2026-10-03T10:00:00Z","transports":[],)" +
                        R"("cluster":{"address":")" + address + R"(","port":5062}})";
    on_strand([this, node, report]() { core->nodes()->observe(events::topics::node_status(node), report); });
  }

  // Bob registered over a WebSocket another node holds: an unresolvable Contact and a flow with no
  // channel here.
  void bind_bob_on(const std::string& node, const std::string& flow = "wss://203.0.113.9:50000",
                   const std::string& contact = "sip:k3j2@n7.invalid;transport=ws") {
    types::Location binding;
    binding.contact = std::make_shared<types::SIPUri>(contact);
    binding.flow_id = flow;
    binding.node_id = node;
    ASSERT_TRUE(store->subscriber_register(bob, binding, 3600));
  }

  // A final response to the INVITE most recently written to that connection.
  static std::string response_to_latest_on(const std::shared_ptr<MockConnection>& connection, int code, const std::string& reason) {
    const auto forwarded = requests_with(connection, "INVITE");
    if (forwarded.empty()) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded.back()->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=bob\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// A call for a flow held elsewhere is forwarded to the holding node, still addressed to Bob.
TEST(ProxyClusterForwardingTest, ACallForAFlowHeldElsewhereGoesToTheNodeThatHoldsIt) {
  ClusterFixture f;
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());

  const auto forwarded = ProxyFixture::requests_with(f.peer_connection, "INVITE");
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded[0]->header->request_uri->to_string(), "sip:bob@example.com");
  EXPECT_EQ(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// A peer is sent one request however many of the subscriber's flows it holds; the peer forks to them.
TEST(ProxyClusterForwardingTest, ANodeHoldingTwoFlowsIsSentOneRequest) {
  ClusterFixture f;
  f.bind_bob_on("node-b", "wss://203.0.113.9:50000", "sip:one@n7.invalid;transport=ws");
  f.bind_bob_on("node-b", "wss://203.0.113.9:50001", "sip:two@n7.invalid;transport=ws");

  f.receive(f.caller, f.invite());

  EXPECT_EQ(ProxyFixture::requests_with(f.peer_connection, "INVITE").size(), 1u);
}

// With one device registered here and one on node-b, the local flow and the peer are both tried.
TEST(ProxyClusterForwardingTest, LocalFlowsAndAPeersAreBothTried) {
  ClusterFixture f;
  f.bind_bob();
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());
  ASSERT_TRUE(ProxyFixture::request_with(f.callee_connection, "INVITE") != nullptr || ProxyFixture::request_with(f.peer_connection, "INVITE") != nullptr);

  // The fork is serial: the second target is tried once the first has refused.
  auto first_local = ProxyFixture::request_with(f.callee_connection, "INVITE") != nullptr;
  f.receive(first_local ? f.callee : f.peer, ClusterFixture::response_to_latest_on(first_local ? f.callee_connection : f.peer_connection, 486, "Busy Here"));

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
  EXPECT_NE(ProxyFixture::request_with(f.peer_connection, "INVITE"), nullptr);
}

// A request from a peer is delivered to this node's own flows only. Forwarding it on would ring
// other devices twice or loop.
TEST(ProxyClusterForwardingTest, ARequestFromAPeerIsDeliveredLocallyAndNeverForwardedOn) {
  ClusterFixture f;
  f.hear("node-c", "ok", "198.51.100.81");
  std::shared_ptr<MockConnection> other_connection;
  auto other = f.make_channel("198.51.100.81", &other_connection, "tls", 5062);
  other_connection->peer = "node-c";

  f.bind_bob();
  f.bind_bob_on("node-c");

  f.receive(f.peer, f.invite());

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
  EXPECT_EQ(ProxyFixture::request_with(other_connection, "INVITE"), nullptr);
  EXPECT_EQ(ProxyFixture::request_with(f.peer_connection, "INVITE"), nullptr);
}

// Nothing is forwarded to a node that is down, stale or unknown: its flows went with it.
TEST(ProxyClusterForwardingTest, NothingIsForwardedToANodeThatIsNotUp) {
  ClusterFixture f;
  f.hear("node-b", "down", "198.51.100.80");
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());

  EXPECT_EQ(ProxyFixture::request_with(f.peer_connection, "INVITE"), nullptr);
}

// The Record-Route towards a peer names this node's inter-node listener, where the peer can reach
// it for the ACK and BYE, not the ephemeral source of the outbound connection.
TEST(ProxyClusterForwardingTest, TheRecordRouteTowardsAPeerNamesTheInterNodeListener) {
  ClusterFixture f;
  f.on_strand([&f]() {
    f.config->cluster_enable = true;
    f.config->cluster_advertise = "10.0.0.1";
    f.config->cluster_port = 5062;
  });
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());

  const auto forwarded = ProxyFixture::request_with(f.peer_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Record-Route"));

  const auto facing_peer = forwarded->header->headers_map["Record-Route"][0]->to_string();
  EXPECT_NE(facing_peer.find("@10.0.0.1:5062"), std::string::npos) << facing_peer;
  EXPECT_NE(facing_peer.find("transport=tls"), std::string::npos) << facing_peer;
}

// Only the node the caller reached writes the call record; a node a peer forwarded to records nothing.
TEST(ProxyClusterForwardingTest, ACallAPeerForwardedIsNotRecordedHere) {
  ClusterFixture f;
  f.bind_bob();

  f.receive(f.peer, f.invite());
  ASSERT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
  f.receive(f.callee, ClusterFixture::response_to_latest_on(f.callee_connection, 180, "Ringing"));

  EXPECT_NE(f.call(), nullptr) << "the node still carries the call";
  EXPECT_TRUE(f.store->call_list().empty());
}

// The call record names the node that held the callee.
TEST(ProxyClusterForwardingTest, TheRecordNamesTheNodeThatHeldTheCallee) {
  ClusterFixture f;
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());
  f.receive(f.peer, ClusterFixture::response_to_latest_on(f.peer_connection, 200, "OK"));

  const auto records = f.store->call_list();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0]->node, "test-node");
  ASSERT_EQ(records[0]->participants.size(), 2u);
  EXPECT_EQ(records[0]->participants[0].node_id, "test-node");
  EXPECT_EQ(records[0]->participants[1].node_id, "node-b");
}
