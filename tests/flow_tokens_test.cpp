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

// RFC 5626 section 5.2: the token in the Record-Route is how an in-dialog request finds its
// way back to the flow it names. A UDP flow is the pair of addresses and nothing more, so
// it outlives this node forgetting it - and the token has to be able to say which flow it
// was even then.
TEST(FlowTokensTest, ASealedFlowOpensToTheFlowItNamed) {
  FlowTokens tokens;

  const auto token = tokens.seal("udp://203.0.113.7:40000");

  EXPECT_EQ(tokens.open(token), "udp://203.0.113.7:40000");
}

TEST(FlowTokensTest, AnIpv6FlowSurvivesTheRoundTrip) {
  FlowTokens tokens;

  EXPECT_EQ(tokens.open(tokens.seal("udp://2001:db8::7:40000")), "udp://2001:db8::7:40000");
}

// The Record-Route goes to both ends of a call this node anchors, and the flow id is the
// far end's address. A token that spelled it out would hand each end the other's.
TEST(FlowTokensTest, TheTokenDoesNotSayWhereAnybodyIs) {
  FlowTokens tokens;

  const auto token = tokens.seal("udp://203.0.113.7:40000");

  EXPECT_EQ(token.find("203.0.113.7"), std::string::npos) << token;
  EXPECT_EQ(token.find("udp"), std::string::npos) << token;
  EXPECT_EQ(token.find("40000"), std::string::npos) << token;
}

// It sits in the user part of a SIP URI (RFC 3261 25.1), so it is made of characters that
// need no escaping there.
TEST(FlowTokensTest, TheTokenIsSafeInTheUserPartOfAUri) {
  FlowTokens tokens;

  const auto token = tokens.seal("ws://198.51.100.20:51234");
  ASSERT_FALSE(token.empty());

  for (const char c : token) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << token;
}

// Two flows, two tokens, and the same flow sealed twice is not recognisable as the same
// flow from outside.
TEST(FlowTokensTest, NoTwoSealsAreAlike) {
  FlowTokens tokens;

  EXPECT_NE(tokens.seal("udp://203.0.113.7:40000"), tokens.seal("udp://203.0.113.7:40000"));
}

// The token comes back in a Route anybody on the path can write. One this node did not
// seal, or one somebody has altered, names nothing - it must not name an address of the
// writer's choosing.
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
