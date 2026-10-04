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
