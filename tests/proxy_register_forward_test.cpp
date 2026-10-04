//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "headers/sip_identity_header.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

// A desk phone of Alice's, a subscriber here, registers with an upstream provider through this node (RFC 3261
// 10.3 step 1), over UDP from 192.0.2.30. The provider's registrar is at 192.0.2.99.
struct ForwardFixture : ProxyFixture {
  std::shared_ptr<MockConnection> upstream_connection;
  std::shared_ptr<Channel> upstream;

  std::shared_ptr<MockConnection> desk_connection;
  std::shared_ptr<Channel> desk;

  ForwardFixture() {
    upstream = make_channel("192.0.2.99", &upstream_connection);
    desk = make_channel("192.0.2.30", &desk_connection);
    desk_connection->reliable = false;
  }

  static std::string foreign_register(const std::string& supported = "path", const std::string& to = "sip:desk@provider.example") {
    std::string raw = "REGISTER sip:192.0.2.99 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.30:5060;branch=z9hG4bK-forward\r\n";
    raw += "From: <" + to + ">;tag=desk\r\n";
    raw += "To: <" + to + ">\r\n";
    raw += "Call-ID: call-forward-register\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:desk@192.0.2.30:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "Expires: 600\r\n";
    if (!supported.empty()) raw += "Supported: " + supported + "\r\n";
    raw += "\r\n";
    return raw;
  }

  std::shared_ptr<SIPMessage> forwarded() {
    const auto found = requests_with(upstream_connection, "REGISTER");
    return found.empty() ? nullptr : found.front();
  }
};

}  // namespace

// RFC 3261 22.3: a sender this node does not know is challenged for this node's realm before anything leaves.
TEST(ProxyRegisterForwardTest, AnUnknownSenderIsChallenged) {
  ForwardFixture f;

  f.receive(f.desk, ForwardFixture::foreign_register());

  auto challenge = ProxyFixture::response_with(f.desk_connection, 407);
  ASSERT_NE(challenge, nullptr);
  EXPECT_NE(challenge->header->headers_map["Proxy-Authenticate"][0]->to_string().find("realm=\"example.com\""), std::string::npos);
  EXPECT_EQ(f.forwarded(), nullptr);
}

// RFC 3261 10.3 step 1: one of this node's subscribers has a REGISTER for another domain forwarded there, over
// UDP as over anything else. Its credentials for this node are spent here; the provider's are left alone.
TEST(ProxyRegisterForwardTest, ASubscribersRegisterForAnotherDomainIsForwarded) {
  ForwardFixture f;

  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(), "alice", "alice-ha1"));

  auto forwarded = f.forwarded();
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->to_string(), "sip:192.0.2.99");
  EXPECT_FALSE(forwarded->header->contains("Proxy-Authorization"));

  // RFC 3261 10.2: Record-Route means nothing in a REGISTER.
  EXPECT_FALSE(forwarded->header->contains("Record-Route"));
}

// RFC 3327 5.2 and RFC 5626 5.1: the first hop puts itself on the Path with a flow token and "ob", so the
// provider's requests come back down the client's flow.
TEST(ProxyRegisterForwardTest, TheFirstHopAddsAPathWithAFlowToken) {
  ForwardFixture f;

  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(), "alice", "alice-ha1"));

  auto forwarded = f.forwarded();
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Path"));

  auto path = forwarded->header->headers_map["Path"][0]->as<headers::SIPIdentityHeader>();
  ASSERT_NE(path, nullptr);
  EXPECT_FALSE(path->value->uri->user.empty());
  EXPECT_TRUE(path->value->uri->has_parameter("lr"));
  EXPECT_TRUE(path->value->uri->has_parameter("ob"));
}

// RFC 3327 5.2: no Path unless the client said it supports one.
TEST(ProxyRegisterForwardTest, NoPathForAClientThatDoesNotSupportIt) {
  ForwardFixture f;

  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(""), "alice", "alice-ha1"));

  auto forwarded = f.forwarded();
  ASSERT_NE(forwarded, nullptr);
  EXPECT_FALSE(forwarded->header->contains("Path"));
}

// A reliable connection a subscriber registered over is trusted for it, as for its calls.
TEST(ProxyRegisterForwardTest, ARegisteredReliableConnectionIsNotChallenged) {
  ForwardFixture f;
  std::shared_ptr<MockConnection> tcp_connection;
  auto tcp = f.make_channel("192.0.2.10", &tcp_connection, "tcp", 50000);
  f.on_strand([&]() { tcp->authenticated_as("sip:alice@example.com"); });

  auto raw = ForwardFixture::foreign_register();
  raw.replace(raw.find("SIP/2.0/UDP 192.0.2.30:5060"), 27, "SIP/2.0/TCP 192.0.2.10:50000");
  f.receive(tcp, raw);

  EXPECT_EQ(ProxyFixture::response_with(tcp_connection, 407), nullptr);
  EXPECT_NE(f.forwarded(), nullptr);
}

// Credentials that verify against no subscriber here are challenged again, like a wrong password.
TEST(ProxyRegisterForwardTest, CredentialsForNoSubscriberAreChallengedAgain) {
  ForwardFixture f;

  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(), "mallory", "mallory-ha1"));

  EXPECT_NE(ProxyFixture::response_with(f.desk_connection, 407), nullptr);
  EXPECT_EQ(f.forwarded(), nullptr);
}

// sip.forward_register: never refuses, and nothing leaves.
TEST(ProxyRegisterForwardTest, NeverRefusesWith403) {
  ForwardFixture f;
  f.config->sip_forward_register = "never";

  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(), "alice", "alice-ha1"));

  EXPECT_NE(ProxyFixture::response_with(f.desk_connection, 403), nullptr);
  EXPECT_EQ(f.forwarded(), nullptr);
}

// A REGISTER for an address of record in one of this node's realms is registered here, whatever its
// Request-URI.
TEST(ProxyRegisterForwardTest, AnAddressOfRecordHereIsRegisteredHere) {
  ForwardFixture f;

  f.receive(f.desk, ForwardFixture::foreign_register("path", "sip:alice@example.com"));

  EXPECT_NE(ProxyFixture::response_with(f.desk_connection, 401), nullptr);
  EXPECT_EQ(f.forwarded(), nullptr);
}

// RFC 3327 5.4 and RFC 5626 5.3: the provider's request, routed through the Path this node wrote, goes down
// the flow the client registered on.
TEST(ProxyRegisterForwardTest, ARequestBackThroughThePathReachesTheClient) {
  ForwardFixture f;
  f.receive(f.desk, f.with_credentials(ForwardFixture::foreign_register(), "alice", "alice-ha1"));
  auto forwarded = f.forwarded();
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Path"));
  const auto path = forwarded->header->headers_map["Path"][0]->to_string();

  std::string invite = "INVITE sip:desk@192.0.2.30:5060 SIP/2.0\r\n";
  invite += "Via: SIP/2.0/UDP 192.0.2.99:5060;branch=z9hG4bK-from-provider\r\n";
  invite += "Route: " + path + "\r\n";
  invite += "From: <sip:someone@provider.example>;tag=provider\r\n";
  invite += "To: <sip:desk@provider.example>\r\n";
  invite += "Call-ID: call-from-provider\r\n";
  invite += "CSeq: 1 INVITE\r\n";
  invite += "Contact: <sip:someone@192.0.2.99:5060>\r\n";
  invite += "Max-Forwards: 70\r\n";
  invite += "\r\n";
  f.receive(f.upstream, invite);

  EXPECT_EQ(ProxyFixture::response_with(f.upstream_connection, 403), nullptr);
  EXPECT_EQ(ProxyFixture::requests_with(f.desk_connection, "INVITE").size(), 1u);
}
