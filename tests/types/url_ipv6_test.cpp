//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/url.h"

using namespace athenasip::types;

// RFC 3986 3.2.2: an IPv6 literal host is in square brackets, which keep its colons from being read as the
// port separator.
TEST(URLIpv6Test, ABracketedLiteralIsTheHostAndTheColonAfterItThePort) {
  URL url("redis://[::1]:6380/3");

  ASSERT_TRUE(url.is_valid());
  EXPECT_EQ(url.host, "::1");
  ASSERT_TRUE(url.port.has_value());
  EXPECT_EQ(*url.port, 6380);
  EXPECT_EQ(url.path, "/3");
}

// The host is handed to drivers to connect to, so it is the address without brackets.
TEST(URLIpv6Test, TheHostIsTheAddressWithoutItsBrackets) {
  URL url("mqtt://[2001:db8::10]");

  ASSERT_TRUE(url.is_valid());
  EXPECT_EQ(url.host, "2001:db8::10");
  ASSERT_TRUE(url.port.has_value());
  EXPECT_EQ(*url.port, 1883);
}

TEST(URLIpv6Test, UserinfoBeforeALiteralStillParses) {
  URL url("redis://:secret@[fe80::1]:6399");

  ASSERT_TRUE(url.is_valid());
  EXPECT_EQ(url.host, "fe80::1");
  ASSERT_TRUE(url.password.has_value());
  EXPECT_EQ(*url.password, "secret");
  EXPECT_EQ(*url.port, 6399);
}

// A host with a colon in it is written back in brackets, so the URL round-trips.
TEST(URLIpv6Test, ALiteralIsWrittenBackInBrackets) {
  URL url("redis://[::1]:6380/3");
  EXPECT_EQ(url.to_string(), "redis://[::1]:6380/3");

  URL round(url.to_string());
  EXPECT_EQ(round, url);
}

TEST(URLIpv6Test, AnUnclosedBracketIsRefused) { EXPECT_FALSE(URL("redis://[::1:6380").is_valid()); }

TEST(URLIpv6Test, SomethingOtherThanAPortAfterTheBracketIsRefused) {
  EXPECT_FALSE(URL("redis://[::1]6380").is_valid());
  EXPECT_FALSE(URL("redis://[::1]:port").is_valid());
}

// RFC 3986 3.2.2: an unbracketed literal is ambiguous, so it is refused rather than guessed at.
TEST(URLIpv6Test, AnUnbracketedLiteralIsRefusedRatherThanGuessed) { EXPECT_FALSE(URL("redis://::1:6379").is_valid()); }

// IPv4 addresses and names need no brackets.
TEST(URLIpv6Test, IPv4AndNamesAreUnchanged) {
  URL v4("redis://127.0.0.1:6399");
  ASSERT_TRUE(v4.is_valid());
  EXPECT_EQ(v4.host, "127.0.0.1");
  EXPECT_EQ(*v4.port, 6399);
  EXPECT_EQ(v4.to_string(), "redis://127.0.0.1:6399");

  URL name("mqtt://broker.example");
  ASSERT_TRUE(name.is_valid());
  EXPECT_EQ(name.host, "broker.example");
  EXPECT_EQ(name.to_string(), "mqtt://broker.example");
}
