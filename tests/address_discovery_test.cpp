//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "address_discovery.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "events/topics.h"
#include "helpers/core_fixture_helper.h"
#include "servers/udp_server.h"
#include "stun.h"

using namespace athenasip;

namespace net = boost::asio;
using udp = boost::asio::ip::udp;

namespace {

// A SIP UDP listener, and a STUN server on loopback that says the listener is at 203.0.113.7:40000, as a router
// would map it.
struct DiscoveryFixture : CoreFixture {
  std::shared_ptr<servers::UDPServer> listener;

  net::io_context io;
  udp::socket stun_server{io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  std::optional<udp::endpoint> asked_from;

  DiscoveryFixture() {
    config->udp_enable = true;
    config->ice_servers.push_back({"stun:127.0.0.1:" + std::to_string(stun_server.local_endpoint().port())});

    listener = std::make_shared<servers::UDPServer>(logger, core, "127.0.0.1", 0);
    listener->start();
    core->server_register(listener);
  }

  ~DiscoveryFixture() {
    on_strand([this]() { core->address_discovery()->stop(); });
    listener->stop();
  }

  // Answers one Binding request, if one comes within the bound.
  bool answer_one(std::chrono::milliseconds bound = std::chrono::seconds(2)) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < until) {
      if (stun_server.available() > 0) {
        std::string data(2048, '\0');
        udp::endpoint sender;
        data.resize(stun_server.receive_from(net::buffer(data), sender));
        asked_from = sender;

        const auto response = stun::binding_response(data, net::ip::make_address("203.0.113.7"), 40000);
        if (!response) return false;
        stun_server.send_to(net::buffer(*response), sender);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  std::optional<AddressDiscovery::Finding> finding() {
    for (int i = 0; i < 200; ++i) {
      if (auto found = on_strand([this]() { return core->address_discovery()->finding(); })) return found;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
  }
};

}  // namespace

// RFC 7064: stun:host[:port], 3478 by default, an IPv6 literal in brackets; turn: entries are not asked.
TEST(AddressDiscoveryTest, ReadsTheStunServersFromTheIceServers) {
  const auto servers = AddressDiscovery::servers_from(
      {"stun:stun.example.org", "stun:192.0.2.5:3479", "stun:[2001:db8::1]:3480", "turn:turn.example.org:3478?transport=udp", "STUN:upper.example.org"});

  ASSERT_EQ(servers.size(), 4u);
  EXPECT_EQ(servers[0], (std::pair<std::string, std::uint16_t>{"stun.example.org", 3478}));
  EXPECT_EQ(servers[1], (std::pair<std::string, std::uint16_t>{"192.0.2.5", 3479}));
  EXPECT_EQ(servers[2], (std::pair<std::string, std::uint16_t>{"2001:db8::1", 3480}));
  EXPECT_EQ(servers[3], (std::pair<std::string, std::uint16_t>{"upper.example.org", 3478}));
}

// The question is asked from the SIP UDP socket, whose mapping is the one that matters, and the answer is what
// the node reports, with the server that gave it.
TEST(AddressDiscoveryTest, AsksFromTheSipSocketAndReportsTheAnswer) {
  DiscoveryFixture f;

  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());

  ASSERT_TRUE(f.asked_from.has_value());
  EXPECT_EQ(f.asked_from->port(), f.listener->local_endpoint().port());

  const auto found = f.finding();
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, "203.0.113.7");
  EXPECT_EQ(found->port, 40000);
  EXPECT_EQ(found->source, "stun:127.0.0.1:" + std::to_string(f.stun_server.local_endpoint().port()));
}

// The node status carries what was found, so another node and the operator see it.
TEST(AddressDiscoveryTest, TheNodeStatusReportsTheFinding) {
  DiscoveryFixture f;
  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());
  ASSERT_TRUE(f.finding().has_value());

  const auto status = boost::json::parse(f.on_strand([&]() { return f.core->node_status_json("ok"); }));
  EXPECT_EQ(status.at("discovered").at("address").as_string(), "203.0.113.7");
}

// With no stun: server nothing is asked, and the status says nothing was found.
TEST(AddressDiscoveryTest, NothingIsAskedWithoutAStunServer) {
  DiscoveryFixture f;
  f.config->ice_servers.clear();

  f.on_strand([&]() { f.core->address_discovery()->start(); });

  EXPECT_FALSE(f.answer_one(std::chrono::milliseconds(200)));
  const auto status = boost::json::parse(f.on_strand([&]() { return f.core->node_status_json("ok"); }));
  EXPECT_TRUE(status.at("discovered").is_null());
}

// A server that does not answer is given up after three seconds and the next one asked.
TEST(AddressDiscoveryTest, AServerThatDoesNotAnswerIsPassedOver) {
  DiscoveryFixture f;
  net::io_context other_io;
  udp::socket silent{other_io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  f.config->ice_servers.insert(f.config->ice_servers.begin(), {"stun:127.0.0.1:" + std::to_string(silent.local_endpoint().port())});

  f.on_strand([&]() { f.core->address_discovery()->start(); });
  EXPECT_FALSE(f.answer_one(std::chrono::milliseconds(200)));

  f.timers->advance(std::chrono::seconds(3));
  f.settle();

  ASSERT_TRUE(f.answer_one());
  EXPECT_TRUE(f.finding().has_value());
}

namespace {

// What node-b says on the bus: that its STUN server sees it at `discovered`, its UDP listener's port, and whose
// addresses it has reached.
void hear_node_b(DiscoveryFixture& f, const std::string& discovered, std::uint16_t udp_port, const std::string& reaches = "") {
  std::string report = R"({"status":"ok","node":"node-b","version":"1.0.0","at":"2026-10-04T10:00:00Z","transports":[)";
  report += R"({"transport":"udp","address":")" + discovered + R"(","port":)" + std::to_string(udp_port) + "}]";
  if (!discovered.empty()) report += R"(,"discovered":{"address":")" + discovered + R"(","port":1,"source":"stun:x"})";
  report += R"(,"reaches":[)" + reaches + "]}";
  f.on_strand([&f, report]() { f.core->nodes()->observe(events::topics::node_status("node-b"), report); });
}

// Answers one OPTIONS on `socket` with a 200 built from it (RFC 3261 8.2.6), and returns its Request-URI.
std::string answer_options(udp::socket& socket, std::chrono::milliseconds bound = std::chrono::seconds(2)) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (socket.available() > 0) {
      std::string data(4096, '\0');
      udp::endpoint sender;
      data.resize(socket.receive_from(net::buffer(data), sender));

      const auto first = data.substr(0, data.find("\r\n"));
      std::string response = "SIP/2.0 200 OK\r\n";
      for (const std::string name : {"Via:", "From:", "To:", "Call-ID:", "CSeq:"}) {
        const auto at = data.find("\r\n" + name);
        if (at == std::string::npos) continue;
        const auto end = data.find("\r\n", at + 2);
        response += data.substr(at + 2, end - at - 2) + (name == "To:" ? ";tag=b" : "") + "\r\n";
      }
      response += "Content-Length: 0\r\n\r\n";
      socket.send_to(net::buffer(response), sender);
      return first.substr(8, first.rfind(' ') - 8);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return "";
}

}  // namespace

// A peer's discovered address is probed with an OPTIONS for that address, on its UDP port; answered, the peer is
// listed as reached, which is how it learns its address is good.
TEST(AddressDiscoveryTest, APeersDiscoveredAddressIsProbedAndReached) {
  DiscoveryFixture f;
  net::io_context peer_io;
  udp::socket peer{peer_io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  hear_node_b(f, "127.0.0.1", peer.local_endpoint().port());

  f.on_strand([&]() { f.core->address_discovery()->review(); });
  EXPECT_EQ(answer_options(peer), "sip:127.0.0.1:" + std::to_string(peer.local_endpoint().port()));

  for (int i = 0; i < 200 && f.on_strand([&]() { return f.core->address_discovery()->reached().empty(); }); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const auto status = boost::json::parse(f.on_strand([&]() { return f.core->node_status_json("ok"); }));
  ASSERT_EQ(status.at("reaches").as_array().size(), 1u);
  EXPECT_EQ(status.at("reaches").as_array()[0].at("node").as_string(), "node-b");
  EXPECT_EQ(status.at("reaches").as_array()[0].at("address").as_string(), "127.0.0.1");
}

// A finding a peer has reached is verified, and with no sip.public_address it is what this node advertises.
TEST(AddressDiscoveryTest, AFindingAPeerReachedIsAdvertised) {
  DiscoveryFixture f;
  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());
  ASSERT_TRUE(f.finding().has_value());

  hear_node_b(f, "", 5060, R"({"node":"test-node","address":"203.0.113.7"})");
  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_EQ(f.on_strand([&]() { return f.core->address_discovery()->verified_by(); }), std::vector<std::string>{"node-b"});
  EXPECT_EQ(f.config->public_address(), "203.0.113.7");
  EXPECT_EQ(f.config->advertised_transports().front().address, "203.0.113.7");

  // A Route naming the address is this node's own (RFC 3261 16.4).
  EXPECT_TRUE(f.on_strand([&]() { return f.core->is_local_address("203.0.113.7", f.config->advertised_transports().front().port); }));
}

// A finding no peer has reached is reported but never advertised.
TEST(AddressDiscoveryTest, AFindingNoPeerReachedIsNotAdvertised) {
  DiscoveryFixture f;
  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());
  ASSERT_TRUE(f.finding().has_value());

  hear_node_b(f, "", 5060);
  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_TRUE(f.config->public_address().empty());
}

// sip.public_address always wins over what was discovered.
TEST(AddressDiscoveryTest, TheConfiguredAddressWins) {
  DiscoveryFixture f;
  f.config->sip_public_address = "198.51.100.1";
  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());
  ASSERT_TRUE(f.finding().has_value());

  hear_node_b(f, "", 5060, R"({"node":"test-node","address":"203.0.113.7"})");
  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_EQ(f.config->public_address(), "198.51.100.1");
}

namespace {

// node-b, in a cluster, with what it found when it tried this node's inter-node listener.
void hear_cluster_peer(DiscoveryFixture& f, const std::string& probes, const std::string& cluster_port = "1") {
  const auto report = R"({"status":"ok","node":"node-b","version":"1.0.0","at":"2026-10-04T10:00:00Z","transports":[],)"
                      R"("cluster":{"address":"127.0.0.1","port":)" +
                      cluster_port + R"(},"cluster_probes":[)" + probes + "]}";
  f.on_strand([&f, report]() { f.core->nodes()->observe(events::topics::node_status("node-b"), report); });
}

void in_a_cluster(DiscoveryFixture& f) {
  f.config->cluster_enable = true;
  f.config->cluster_address = "192.0.2.1";
  f.config->cluster_port = 5062;
}

}  // namespace

// Each peer's inter-node listener is tried as forwarding would try it, and the result published.
TEST(AddressDiscoveryTest, APeersInterNodeListenerIsTriedAndTheResultPublished) {
  DiscoveryFixture f;
  in_a_cluster(f);
  hear_cluster_peer(f, "");

  f.on_strand([&]() { f.core->address_discovery()->review(); });

  for (int i = 0; i < 400 && f.on_strand([&]() { return f.core->address_discovery()->cluster_probes().empty(); }); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // This fixture has no cluster certificates, so the attempt fails, and says so.
  const auto status = boost::json::parse(f.on_strand([&]() { return f.core->node_status_json("ok"); }));
  ASSERT_EQ(status.at("cluster_probes").as_array().size(), 1u);
  EXPECT_EQ(status.at("cluster_probes").as_array()[0].at("node").as_string(), "node-b");
  EXPECT_FALSE(status.at("cluster_probes").as_array()[0].at("reached").as_bool());
}

// A peer that tried this node's inter-node listener and failed, with none succeeding, marks it unreachable, and
// the status says so for peers to stop forwarding.
TEST(AddressDiscoveryTest, AListenerNoPeerReachesIsMarkedUnreachable) {
  DiscoveryFixture f;
  in_a_cluster(f);
  hear_cluster_peer(f, R"({"node":"test-node","reached":false})");

  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_TRUE(f.on_strand([&]() { return f.core->address_discovery()->cluster_unreachable(); }));
  const auto status = boost::json::parse(f.on_strand([&]() { return f.core->node_status_json("ok"); }));
  EXPECT_FALSE(status.at("cluster").at("reachable").as_bool());
  EXPECT_EQ(status.at("cluster").at("address").as_string(), "192.0.2.1");
}

// One peer getting through is enough.
TEST(AddressDiscoveryTest, AListenerAPeerReachesIsNotMarked) {
  DiscoveryFixture f;
  in_a_cluster(f);
  hear_cluster_peer(f, R"({"node":"test-node","reached":true})");

  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_FALSE(f.on_strand([&]() { return f.core->address_discovery()->cluster_unreachable(); }));
}

// Before any peer has tried, nothing is concluded.
TEST(AddressDiscoveryTest, NothingIsConcludedBeforeAPeerHasTried) {
  DiscoveryFixture f;
  in_a_cluster(f);
  hear_cluster_peer(f, "");

  f.on_strand([&]() { f.core->address_discovery()->review(); });

  EXPECT_FALSE(f.on_strand([&]() { return f.core->address_discovery()->cluster_unreachable(); }));
}

// RFC 3581: a public address a peer saw this node's request come from is a finding when no STUN server has
// answered.
TEST(AddressDiscoveryTest, APublicAddressAPeerSawIsAFinding) {
  DiscoveryFixture f;

  f.on_strand([&]() { f.core->address_discovery()->observed("node-b", "203.0.113.50"); });

  const auto found = f.finding();
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, "203.0.113.50");
  EXPECT_EQ(found->source, "peer:node-b");
}

// Nodes on one LAN see each other's private addresses, which say nothing about where the node is.
TEST(AddressDiscoveryTest, APrivateAddressAPeerSawIsNot) {
  DiscoveryFixture f;

  f.on_strand([&]() {
    for (const auto* address : {"10.35.1.20", "172.20.0.5", "192.168.1.2", "100.64.0.1", "169.254.1.1", "127.0.0.1", "fd00::1", "fe80::1"}) {
      f.core->address_discovery()->observed("node-b", address);
    }
  });

  EXPECT_FALSE(f.on_strand([&]() { return f.core->address_discovery()->finding(); }).has_value());
}

// A STUN answer, asked from the SIP socket itself, stands over what a peer saw.
TEST(AddressDiscoveryTest, AStunAnswerStandsOverWhatAPeerSaw) {
  DiscoveryFixture f;
  f.on_strand([&]() { f.core->address_discovery()->start(); });
  ASSERT_TRUE(f.answer_one());
  ASSERT_TRUE(f.finding().has_value());

  f.on_strand([&]() { f.core->address_discovery()->observed("node-b", "198.51.100.9"); });

  EXPECT_EQ(f.finding()->address, "203.0.113.7");
}
