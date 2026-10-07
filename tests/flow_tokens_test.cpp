//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "flow_tokens.h"

#include <gtest/gtest.h>

#include <string>

using namespace athenasip;

// RFC 5626 section 5.2: the Record-Route token names the flow an in-dialog request returns to. It
// carries the flow id itself, so it still opens after the node has forgotten a UDP flow.
TEST(FlowTokensTest, ASealedFlowOpensToTheFlowItNamed) {
  FlowTokens tokens;

  const auto token = tokens.seal("udp://203.0.113.7:40000");

  EXPECT_EQ(tokens.open(token), "udp://203.0.113.7:40000");
}

TEST(FlowTokensTest, AnIpv6FlowSurvivesTheRoundTrip) {
  FlowTokens tokens;

  EXPECT_EQ(tokens.open(tokens.seal("udp://2001:db8::7:40000")), "udp://2001:db8::7:40000");
}

// The Record-Route reaches both ends of a call, so the token must not reveal the flow's address.
TEST(FlowTokensTest, TheTokenDoesNotSayWhereAnybodyIs) {
  FlowTokens tokens;

  const auto token = tokens.seal("udp://203.0.113.7:40000");

  EXPECT_EQ(token.find("203.0.113.7"), std::string::npos) << token;
  EXPECT_EQ(token.find("udp"), std::string::npos) << token;
  EXPECT_EQ(token.find("40000"), std::string::npos) << token;
}

// The token is the user part of a SIP URI (RFC 3261 25.1) and uses only characters that need no escaping.
TEST(FlowTokensTest, TheTokenIsSafeInTheUserPartOfAUri) {
  FlowTokens tokens;

  const auto token = tokens.seal("ws://198.51.100.20:51234");
  ASSERT_FALSE(token.empty());

  for (const char c : token) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << token;
}

// Sealing the same flow twice gives different tokens.
TEST(FlowTokensTest, NoTwoSealsAreAlike) {
  FlowTokens tokens;

  EXPECT_NE(tokens.seal("udp://203.0.113.7:40000"), tokens.seal("udp://203.0.113.7:40000"));
}

// A Route is writable by anybody on the path: a token that was altered, or that this node did not
// seal, opens to nothing.
TEST(FlowTokensTest, AnAlteredTokenOpensToNothing) {
  FlowTokens tokens;

  auto token = tokens.seal("udp://203.0.113.7:40000");
  token[token.size() / 2] = token[token.size() / 2] == '0' ? '1' : '0';

  EXPECT_EQ(tokens.open(token), "");
}

TEST(FlowTokensTest, AnotherNodesTokenOpensToNothing) {
  FlowTokens ours;
  FlowTokens theirs;

  EXPECT_EQ(ours.open(theirs.seal("udp://203.0.113.7:40000")), "");
}

TEST(FlowTokensTest, WhatIsNotATokenOpensToNothing) {
  FlowTokens tokens;

  EXPECT_EQ(tokens.open(""), "");
  EXPECT_EQ(tokens.open("f0123456789abcdefghij"), "");
  EXPECT_EQ(tokens.open("abc"), "");
  EXPECT_EQ(tokens.open(std::string(64, '0')), "");
}
