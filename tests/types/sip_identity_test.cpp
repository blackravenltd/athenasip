//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/sip_identity.h"

using athenasip::types::SIPIdentity;

// RFC 3261 20.10, 20.20, 25.1: (name-addr | addr-spec) *(SEMI param)

TEST(SIPIdentityTest, ParsesNameAddrWithQuotedDisplayName) {
  SIPIdentity identity("\"Alice Smith\" <sip:alice@example.com>;tag=abc123");

  ASSERT_TRUE(identity.display_name.has_value());
  EXPECT_EQ(identity.display_name.value(), "Alice Smith");
  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.uri->user, "alice");
  EXPECT_EQ(identity.uri->realm, "example.com");
  EXPECT_EQ(identity.tags["tag"], "abc123");
}

TEST(SIPIdentityTest, ParsesNameAddrWithoutDisplayName) {
  SIPIdentity identity("<sip:bob@example.com>;tag=xyz");

  EXPECT_FALSE(identity.display_name.has_value());
  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.uri->user, "bob");
  EXPECT_EQ(identity.tags["tag"], "xyz");
}

// RFC 3261 20: with no angle brackets, a semicolon after the URI starts a header
// parameter. The tag must not end up inside the URI.
TEST(SIPIdentityTest, AddrSpecParametersAreHeaderParametersNotUriParameters) {
  SIPIdentity identity("sip:carol@example.com;tag=qwerty");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.uri->user, "carol");
  EXPECT_EQ(identity.uri->realm, "example.com");
  EXPECT_EQ(identity.uri->parameters, "");
  EXPECT_EQ(identity.tags["tag"], "qwerty");
}

// With angle brackets, the same semicolon inside them is a URI parameter.
TEST(SIPIdentityTest, NameAddrKeepsUriParametersInsideTheBrackets) {
  SIPIdentity identity("<sip:dave@example.com;transport=tcp>;tag=zzz");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.uri->parameters, "transport=tcp");
  EXPECT_EQ(identity.tags["tag"], "zzz");
  EXPECT_EQ(identity.tags.count("transport"), 0u);
}

TEST(SIPIdentityTest, RoundTripsNameAddr) {
  SIPIdentity identity("\"Alice\" <sip:alice@example.com>;tag=abc");
  const auto text = identity.to_string();

  EXPECT_NE(text.find("<sip:alice@example.com>"), std::string::npos);
  EXPECT_NE(text.find(";tag=abc"), std::string::npos);
  EXPECT_NE(text.find("\"Alice\""), std::string::npos);
}
