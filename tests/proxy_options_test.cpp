//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

std::string options(const std::string& uri, const std::string& max_forwards = "70") {
  std::string raw = "OPTIONS " + uri + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-options\r\n";
  raw += "From: <sip:monitor@monitor.example>;tag=monitor\r\n";
  raw += "To: <" + uri + ">\r\n";
  raw += "Call-ID: call-options\r\n";
  raw += "CSeq: 1 OPTIONS\r\n";
  raw += "Max-Forwards: " + max_forwards + "\r\n";
  raw += "\r\n";
  return raw;
}

}  // namespace

// RFC 3261 11.2: an OPTIONS for this node is answered by it, with what it allows, accepts and supports, so a
// monitor or a trunk can tell it is up.
TEST(ProxyOptionsTest, AnOptionsForThisNodeIsAnsweredByIt) {
  ProxyFixture f;

  f.receive(f.caller, options("sip:192.0.2.1:5060"));

  auto answer = ProxyFixture::response_with(f.caller_connection, 200);
  ASSERT_NE(answer, nullptr);
  ASSERT_TRUE(answer->header->contains("Allow"));
  std::vector<std::string> allow;
  for (const auto& value : answer->header->headers_map["Allow"]) allow.push_back(value->to_string());
  for (const std::string method : {"INVITE", "ACK", "CANCEL", "BYE", "OPTIONS", "REGISTER"}) {
    EXPECT_EQ(std::count(allow.begin(), allow.end(), method), 1) << method;
  }
  EXPECT_TRUE(answer->header->contains("Accept"));
  EXPECT_TRUE(answer->header->contains("Supported"));
}

// A realm this node serves, with no user, is this node too.
TEST(ProxyOptionsTest, AnOptionsForARealmHereIsAnsweredByThisNode) {
  ProxyFixture f;

  f.receive(f.caller, options("sip:example.com"));

  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 200), nullptr);
}

// sip.public_address names this node as well, whatever the socket's own address.
TEST(ProxyOptionsTest, TheConfiguredPublicAddressIsThisNode) {
  ProxyFixture f("203.0.113.5");

  f.receive(f.caller, options("sip:203.0.113.5"));

  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 200), nullptr);
}

// An OPTIONS for a subscriber goes to the subscriber.
TEST(ProxyOptionsTest, AnOptionsForASubscriberIsForwarded) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, options("sip:bob@example.com"));

  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 200), nullptr);
  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "OPTIONS").size(), 1u);
}

// RFC 3261 16.3 step 3: an OPTIONS that has run out of hops may be answered as a UAS rather than refused.
TEST(ProxyOptionsTest, AnOptionsWithNoHopsLeftIsAnsweredRatherThanRefused) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, options("sip:bob@example.com", "0"));

  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 200), nullptr);
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 483), nullptr);
  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "OPTIONS").empty());
}
