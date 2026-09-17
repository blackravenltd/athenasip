//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/sip_uri.h"

using athenasip::types::SIPUri;

// RFC 3261 19.1.1: sip:user:password@host:port;uri-parameters?headers
TEST(SIPUriTest, ParsesFullUri) {
  SIPUri uri("sip:alice:secret@example.com:5070;transport=tcp?subject=hi");

  EXPECT_TRUE(uri.valid);
  EXPECT_EQ(uri.scheme, "sip");
  EXPECT_EQ(uri.user, "alice");
  ASSERT_TRUE(uri.password.has_value());
  EXPECT_EQ(uri.password.value(), "secret");
  EXPECT_EQ(uri.realm, "example.com");
  ASSERT_TRUE(uri.port.has_value());
  EXPECT_EQ(uri.port.value(), 5070);
}

TEST(SIPUriTest, ParsesMinimalUri) {
  SIPUri uri("sip:example.com");

  EXPECT_TRUE(uri.valid);
  EXPECT_EQ(uri.scheme, "sip");
  EXPECT_EQ(uri.user, "");
  EXPECT_EQ(uri.realm, "example.com");
  EXPECT_FALSE(uri.port.has_value());
}

TEST(SIPUriTest, ParsesSipsScheme) {
  SIPUri uri("sips:alice@example.com");

  EXPECT_TRUE(uri.valid);
  EXPECT_EQ(uri.scheme, "sips");
}

// RFC 3261 19.1.1 and RFC 5118 4: an IPv6 host is a reference in square brackets, and
// the colons inside it are not a port separator.
TEST(SIPUriTest, ParsesIPv6HostReference) {
  SIPUri uri("sip:alice@[2001:db8::1]");

  EXPECT_TRUE(uri.valid);
  EXPECT_EQ(uri.user, "alice");
  EXPECT_EQ(uri.realm, "[2001:db8::1]");
  EXPECT_FALSE(uri.port.has_value());
}

TEST(SIPUriTest, ParsesIPv6HostReferenceWithPort) {
  SIPUri uri("sip:alice@[2001:db8::1]:5061");

  EXPECT_TRUE(uri.valid);
  EXPECT_EQ(uri.realm, "[2001:db8::1]");
  ASSERT_TRUE(uri.port.has_value());
  EXPECT_EQ(uri.port.value(), 5061);
}

// A port outside the 16-bit range is not a valid URI, and must not wrap around.
TEST(SIPUriTest, RejectsOutOfRangePort) {
  SIPUri uri("sip:alice@example.com:70000");
  EXPECT_FALSE(uri.valid);
}

TEST(SIPUriTest, DoesNotThrowOnAbsurdPort) {
  EXPECT_NO_THROW({ SIPUri uri("sip:alice@example.com:999999999999999999999"); });
}

TEST(SIPUriTest, InvalidUriIsMarkedInvalid) {
  EXPECT_FALSE(SIPUri("not a uri at all").valid);
  EXPECT_FALSE(SIPUri("http://example.com").valid);
  EXPECT_FALSE(SIPUri("").valid);
}

TEST(SIPUriTest, RoundTrips) {
  const std::string text = "sip:alice@example.com:5070;transport=tcp";
  EXPECT_EQ(SIPUri(text).to_string(), text);

  const std::string simple = "sip:bob@example.com";
  EXPECT_EQ(SIPUri(simple).to_string(), simple);
}
