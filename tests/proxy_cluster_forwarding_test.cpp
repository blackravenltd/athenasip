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

// Two nodes and one datastore, seen from node "test-node". The 2026-09-20 decision: a
// binding shares and a flow does not, so a node that reads a binding whose flow another
// node holds forwards to that node, and that node delivers down the flow.
struct ClusterFixture : ProxyFixture {
  std::shared_ptr<MockConnection> peer_connection;
  std::shared_ptr<Channel> peer;

  ClusterFixture() {
    // node-b, as it describes itself on the bus, and the mutual-TLS flow to it.
    hear("node-b", "ok", "198.51.100.80");
    peer = make_channel("198.51.100.80", &peer_connection, "tls", 5062);
    peer_connection->peer = "node-b";
  }

  void hear(const std::string& node, const std::string& state, const std::string& address) {
    const auto report = R"({"status":")" + state + R"(","node":")" + node + R"(","version":"1.0.0","at":"2026-10-03T10:00:00Z","transports":[],)" +
                        R"("cluster":{"address":")" + address + R"(","port":5062}})";
    on_strand([this, node, report]() { core->nodes()->observe(events::topics::node_status(node), report); });
  }

  // Bob registered over a WebSocket that another node holds: a Contact that resolves to
  // nothing, and a flow this node has no channel for.
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

// The call goes to the node that holds the flow, still addressed to Bob: that node reads
// the same bindings and knows which of them are its own to deliver.
TEST(ProxyClusterForwardingTest, ACallForAFlowHeldElsewhereGoesToTheNodeThatHoldsIt) {
  ClusterFixture f;
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());

  const auto forwarded = ProxyFixture::requests_with(f.peer_connection, "INVITE");
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded[0]->header->request_uri->to_string(), "sip:bob@example.com");
  EXPECT_EQ(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// One request per node, however many of the subscriber's devices that node holds: the
// peer forks to its own flows, and two copies would ring each of them twice.
TEST(ProxyClusterForwardingTest, ANodeHoldingTwoFlowsIsSentOneRequest) {
  ClusterFixture f;
  f.bind_bob_on("node-b", "wss://203.0.113.9:50000", "sip:one@n7.invalid;transport=ws");
  f.bind_bob_on("node-b", "wss://203.0.113.9:50001", "sip:two@n7.invalid;transport=ws");

  f.receive(f.caller, f.invite());

  EXPECT_EQ(ProxyFixture::requests_with(f.peer_connection, "INVITE").size(), 1u);
}

// A device registered here and another on node-b: the local one is offered the call down
// its own flow, and node-b is sent the request for the other.
TEST(ProxyClusterForwardingTest, LocalFlowsAndAPeersAreBothTried) {
  ClusterFixture f;
  f.bind_bob();
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());
  ASSERT_TRUE(ProxyFixture::request_with(f.callee_connection, "INVITE") != nullptr || ProxyFixture::request_with(f.peer_connection, "INVITE") != nullptr);

  // The fork is serial, so the second is tried when the first has refused.
  auto first_local = ProxyFixture::request_with(f.callee_connection, "INVITE") != nullptr;
  f.receive(first_local ? f.callee : f.peer, ClusterFixture::response_to_latest_on(first_local ? f.callee_connection : f.peer_connection, 486, "Busy Here"));

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
  EXPECT_NE(ProxyFixture::request_with(f.peer_connection, "INVITE"), nullptr);
}

// What a peer sends is delivered to this node's own flows and never sent on to another
// node. A peer forwards because it read a binding held here; sending it back out to
// whoever holds the subscriber's other bindings is that peer's job, and doing it here too
// would ring those devices twice or send the request round in a circle.
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

// A node that has said it is down, or has gone quiet, or was never heard from, is not one
// to forward to: the flow it held went with it, and the client will register again
// somewhere that is up.
TEST(ProxyClusterForwardingTest, NothingIsForwardedToANodeThatIsNotUp) {
  ClusterFixture f;
  f.hear("node-b", "down", "198.51.100.80");
  f.bind_bob_on("node-b");

  f.receive(f.caller, f.invite());

  EXPECT_EQ(ProxyFixture::request_with(f.peer_connection, "INVITE"), nullptr);
}

// The Record-Route this node writes towards a peer names its inter-node listener, which is
// where the peer can reach it again for the ACK and the BYE. The address of the connection
// the request went out on would be a port nothing listens on once that connection closes.
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
