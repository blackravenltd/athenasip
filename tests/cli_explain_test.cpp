//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_explain.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "helpers/core_fixture_helper.h"

namespace {

using athenasip::cli::explain;
using athenasip::cli::ExplainSource;
using athenasip::cli::parse_explain_source;

struct ExplainFixture : CoreFixture {
  std::shared_ptr<athenasip::types::Subscriber> alice;
  std::shared_ptr<athenasip::types::Subscriber> bob;

  ExplainFixture() {
    seed_realm("example.com");
    alice = seed_subscriber(1, "sip:alice@example.com", "ha1");
    bob = seed_subscriber(2, "sip:bob@example.com", "ha1");
  }

  athenasip::cli::Explained run(const std::string& request, ExplainSource source = {}) { return explain(logger, core, request, source); }
};

// A file written by hand: bare line feeds and no blank line at the end.
const std::string kInvite =
    "INVITE sip:bob@example.com SIP/2.0\n"
    "Via: SIP/2.0/UDP 192.0.2.1;branch=z9hG4bK-explain\n"
    "Max-Forwards: 70\n"
    "From: <sip:alice@example.com>;tag=a\n"
    "To: <sip:bob@example.com>\n"
    "Call-ID: explain\n"
    "CSeq: 1 INVITE\n";

}  // namespace

// RFC 3261 22.3: a caller in a realm here proves itself, and the call goes to the callee's devices (16.5).
TEST(CliExplainTest, ACallBetweenSubscribersIsChallengedAndGoesToTheCalleesDevices) {
  ExplainFixture f;
  auto device = f.make_channel("192.0.2.20");
  ASSERT_TRUE(f.register_binding(f.bob, std::make_shared<athenasip::types::SIPUri>("sip:bob@192.0.2.20:5060"), device, 3600));

  const auto explained = f.run(kInvite);
  EXPECT_TRUE(explained.ok) << explained.text;
  EXPECT_NE(explained.text.find("authorize  digest: challenged for realm example.com"), std::string::npos) << explained.text;
  EXPECT_NE(explained.text.find("subscriber sip:bob@example.com, 1 binding:"), std::string::npos) << explained.text;
  EXPECT_NE(explained.text.find("sip:bob@192.0.2.20:5060"), std::string::npos) << explained.text;
}

// RFC 3261 16.5: an address of record with no bindings is 480.
TEST(CliExplainTest, ACalleeWithNoDevicesIsSaidToGet480) {
  ExplainFixture f;
  const auto explained = f.run(kInvite);
  EXPECT_TRUE(explained.ok) << explained.text;
  EXPECT_NE(explained.text.find("route      reply 480"), std::string::npos) << explained.text;
}

TEST(CliExplainTest, ARequestNeitherEndOfWhichIsHereIsRefused) {
  ExplainFixture f;
  auto request = kInvite;
  for (const auto& [from, to] : {std::pair<std::string, std::string>{"alice@example.com", "carol@elsewhere.example"},
                                 std::pair<std::string, std::string>{"bob@example.com", "dave@elsewhere.example"}}) {
    for (auto at = request.find(from); at != std::string::npos; at = request.find(from)) request.replace(at, from.size(), to);
  }

  const auto explained = f.run(request);
  EXPECT_TRUE(explained.ok) << explained.text;
  EXPECT_NE(explained.text.find("403 Forbidden"), std::string::npos) << "the node relays for nobody\n" << explained.text;
}

// RFC 3261 10.3: a REGISTER for a realm here is the registrar's, in that realm.
TEST(CliExplainTest, ARegisterForARealmHereIsAccepted) {
  ExplainFixture f;
  const auto explained = f.run(
      "REGISTER sip:example.com SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.1;branch=z9hG4bK-r\r\nFrom: <sip:alice@example.com>;tag=r\r\n"
      "To: <sip:alice@example.com>\r\nCall-ID: r\r\nCSeq: 1 REGISTER\r\nContact: <sip:alice@192.0.2.1>\r\n\r\n");
  EXPECT_TRUE(explained.ok) << explained.text;
  EXPECT_NE(explained.text.find("register   accept in realm example.com"), std::string::npos) << explained.text;
}

// RFC 3261 16.4 and 12.2: a Route, an ACK or a request inside a dialog is routed without the policy.
TEST(CliExplainTest, WhatTheNodeRoutesItselfIsNotPutToThePolicy) {
  ExplainFixture f;
  for (const auto& [request, says] : {std::pair<std::string, std::string>{kInvite + "Route: <sip:192.0.2.99;lr>\n", "It has a Route"},
                                      std::pair<std::string, std::string>{"ACK sip:bob@example.com SIP/2.0\nVia: SIP/2.0/UDP 192.0.2.1;branch=z9hG4bK-a\n"
                                                                          "From: <sip:alice@example.com>;tag=a\nTo: <sip:bob@example.com>\n"
                                                                          "Call-ID: a\nCSeq: 1 ACK\n",
                                                                          "asks the policy nothing"},
                                      std::pair<std::string, std::string>{"BYE sip:bob@192.0.2.20 SIP/2.0\nVia: SIP/2.0/UDP 192.0.2.1;branch=z9hG4bK-b\n"
                                                                          "From: <sip:alice@example.com>;tag=a\nTo: <sip:bob@example.com>;tag=b\n"
                                                                          "Call-ID: b\nCSeq: 2 BYE\n",
                                                                          "inside a dialog"}}) {
    const auto explained = f.run(request);
    EXPECT_TRUE(explained.ok) << explained.text;
    EXPECT_NE(explained.text.find(says), std::string::npos) << explained.text;
    EXPECT_EQ(explained.text.find("authorize"), std::string::npos) << explained.text;
  }
}

TEST(CliExplainTest, SomethingThatIsNotARequestIsSaidSo) {
  ExplainFixture f;
  EXPECT_FALSE(f.run("SIP/2.0 200 OK\nVia: SIP/2.0/UDP 192.0.2.1;branch=z9hG4bK-x\n").ok);
  EXPECT_FALSE(f.run("hello\n").ok);
}

TEST(CliExplainTest, TheSourceIsReadAsTransportAddressAndPort) {
  const auto full = parse_explain_source("tls:203.0.113.5:5061");
  ASSERT_TRUE(full);
  EXPECT_EQ(full->transport, "tls");
  EXPECT_EQ(full->address, "203.0.113.5");
  EXPECT_EQ(full->port, 5061);

  const auto bare = parse_explain_source("203.0.113.5");
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->transport, "udp");
  EXPECT_EQ(bare->port, 5060);

  const auto six = parse_explain_source("tcp:[2001:db8::1]:5070");
  ASSERT_TRUE(six);
  EXPECT_EQ(six->address, "2001:db8::1");
  EXPECT_EQ(six->port, 5070);

  EXPECT_FALSE(parse_explain_source("udp:not-an-address"));
  EXPECT_FALSE(parse_explain_source("udp:203.0.113.5:70000"));
}
