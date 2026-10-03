//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "channel.h"
#include "helpers/core_fixture_helper.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

// A node at 192.168.1.2 on its LAN, reached from outside at 203.0.113.5 through a router
// forwarding public UDP 5080 to local 5060.
struct AdvertisedFixture : CoreFixture {
  AdvertisedFixture() {
    config->sip_public_address = "203.0.113.5";
    config->udp_public_port = 5080;
    config->sip_localnet = {"192.168.0.0/16"};
    EXPECT_TRUE(config->parse_localnet());
  }

  std::shared_ptr<Channel> channel_from(const std::string& remote, const std::string& local = "192.168.1.2", const std::string& transport = "udp") {
    auto connection = std::make_shared<MockConnection>(transport, remote, 5060, local, 5060);
    auto channel = std::make_shared<Channel>(logger, core, connection);
    on_strand([&channel]() { channel->start(); });
    return channel;
  }

  Core::Advertised advertised(const std::shared_ptr<Channel>& channel) {
    return on_strand([this, &channel]() { return core->advertised_for(*channel); });
  }
};

}  // namespace

// Outside the localnet the node is what the router makes it: the public address and the
// port forwarded to this listener, which is not the port it is bound to.
TEST(AdvertisedAddressTest, AFarEndOutsideTheLanIsGivenThePublicAddressAndPort) {
  AdvertisedFixture f;

  const auto seen = f.advertised(f.channel_from("198.51.100.9"));
  EXPECT_EQ(seen.host, "203.0.113.5");
  EXPECT_EQ(seen.port, 5080);
}

// Inside it, the public address only works if the router hairpins, and plenty do not, so a
// client on the same LAN is given the node's own address (Asterisk's localnet).
TEST(AdvertisedAddressTest, AFarEndOnTheLanIsGivenTheLocalAddressAndPort) {
  AdvertisedFixture f;

  const auto seen = f.advertised(f.channel_from("192.168.1.20"));
  EXPECT_EQ(seen.host, "192.168.1.2");
  EXPECT_EQ(seen.port, 5060);
}

// A listener bound to the wildcard knows no address of its own, and 0.0.0.0 in a Via is a
// route nobody can use. The address it would send to that peer from is the one to give.
TEST(AdvertisedAddressTest, AWildcardBindIsResolvedToTheInterfaceThatReachesThePeer) {
  AdvertisedFixture f;
  f.config->sip_localnet = {"127.0.0.0/8"};
  ASSERT_TRUE(f.config->parse_localnet());

  const auto seen = f.advertised(f.channel_from("127.0.0.1", "0.0.0.0"));
  EXPECT_EQ(seen.host, "127.0.0.1");
}

// With no public address configured there is nothing to choose between, and the node is what
// it is bound as, as before.
TEST(AdvertisedAddressTest, WithNoPublicAddressTheLocalOneIsUsed) {
  AdvertisedFixture f;
  f.config->sip_public_address.clear();

  const auto seen = f.advertised(f.channel_from("198.51.100.9"));
  EXPECT_EQ(seen.host, "192.168.1.2");
  EXPECT_EQ(seen.port, 5060);
}

// A port nobody forwarded differently is the bound port.
TEST(AdvertisedAddressTest, WithNoPublicPortTheBoundPortIsUsed) {
  AdvertisedFixture f;
  f.config->udp_public_port = 0;

  EXPECT_EQ(f.advertised(f.channel_from("198.51.100.9")).port, 5060);
}

// In a call each end is told the address that reaches the node from where it is: the
// callee on the LAN the local one in the Via and in the Record-Route facing it, and the
// caller outside the public one in the Record-Route facing it (RFC 5658 section 3).
TEST(AdvertisedAddressTest, EachEndOfACallIsGivenTheAddressThatReachesItsSide) {
  ProxyFixture f("203.0.113.5");
  f.config->udp_public_port = 5080;
  f.config->sip_localnet = {"192.0.2.20/32"};
  EXPECT_TRUE(f.config->parse_localnet());
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_NE(forwarded->header->headers_map["Via"][0]->to_string().find("192.0.2.1:5060"), std::string::npos)
      << forwarded->header->headers_map["Via"][0]->to_string();

  const auto& routes = forwarded->header->headers_map["Record-Route"];
  ASSERT_EQ(routes.size(), 2u);
  EXPECT_NE(routes[0]->to_string().find("192.0.2.1:5060"), std::string::npos) << routes[0]->to_string();
  EXPECT_NE(routes[1]->to_string().find("203.0.113.5:5080"), std::string::npos) << routes[1]->to_string();
}
