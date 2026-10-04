//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/sip_uri.h"

using athenasip::types::SIPUri;

// RFC 3261 19.1: the grammar (19.1.1), escaping (19.1.2, 25.1) and comparison (19.1.4).

// ---------------------------------------------------------------- 19.1.1 parameters

// RFC 3261 19.1.1: uri-parameters are read by name; routing needs lr, transport and maddr.
TEST(SIPUriRFCTest, ParametersAreReadableByName) {
  SIPUri uri("sip:alice@example.com;transport=tcp;maddr=239.255.255.1;ttl=15");

  ASSERT_TRUE(uri.valid);
  EXPECT_EQ(uri.parameter("transport"), "tcp");
  EXPECT_EQ(uri.parameter("maddr"), "239.255.255.1");
  EXPECT_EQ(uri.parameter("ttl"), "15");
}

// RFC 3261 19.1.1: lr has no value, so present-but-empty must be distinguishable from absent.
TEST(SIPUriRFCTest, AValuelessParameterIsPresentWithAnEmptyValue) {
  SIPUri uri("sip:proxy.example.com;lr");

  ASSERT_TRUE(uri.valid);
  EXPECT_TRUE(uri.has_parameter("lr"));
  EXPECT_EQ(uri.parameter("lr"), "");
  EXPECT_FALSE(uri.has_parameter("transport"));
}

// RFC 3261 19.1.1: parameter names are case-insensitive, as are the values of the
// parameters the RFC defines.
TEST(SIPUriRFCTest, ParameterNamesAreCaseInsensitive) {
  SIPUri uri("sip:alice@example.com;Transport=TCP;LR");

  ASSERT_TRUE(uri.valid);
  EXPECT_TRUE(uri.has_parameter("transport"));
  EXPECT_TRUE(uri.has_parameter("lr"));
  EXPECT_EQ(uri.parameter("TRANSPORT"), "TCP");
}

// RFC 3261 19.1.1: headers come after '?' and are separated by '&'.
TEST(SIPUriRFCTest, HeadersAreReadableByName) {
  SIPUri uri("sip:alice@example.com?Subject=project&Priority=urgent");

  ASSERT_TRUE(uri.valid);
  EXPECT_EQ(uri.header("Subject"), "project");
  EXPECT_EQ(uri.header("Priority"), "urgent");
  EXPECT_FALSE(uri.has_header("Nothing"));
}

// ------------------------------------------------------------------ 19.1.2 escaping

// RFC 3261 19.1.2 and 25.1: a character outside the set allowed for a component is
// percent-escaped. The parsed value is the unescaped one.
TEST(SIPUriRFCTest, EscapedCharactersAreDecodedInTheUser) {
  SIPUri uri("sip:alice%20smith@example.com");

  ASSERT_TRUE(uri.valid);
  EXPECT_EQ(uri.user, "alice smith");
}

TEST(SIPUriRFCTest, EscapedCharactersAreDecodedInParametersAndHeaders) {
  SIPUri uri("sip:alice@example.com;note=a%3Bb?Subject=project%20x");

  ASSERT_TRUE(uri.valid);
  EXPECT_EQ(uri.parameter("note"), "a;b");
  EXPECT_EQ(uri.header("Subject"), "project x");
}

// Serialising re-escapes, so the URI sent is the one read.
TEST(SIPUriRFCTest, SerialisingReEscapes) {
  SIPUri uri("sip:alice%20smith@example.com;note=a%3Bb");

  ASSERT_TRUE(uri.valid);
  const auto text = uri.to_string();

  EXPECT_NE(text.find("alice%20smith"), std::string::npos);
  EXPECT_NE(text.find("a%3Bb"), std::string::npos);

  // And what we produced parses back to the same thing.
  SIPUri round_trip(text);
  ASSERT_TRUE(round_trip.valid);
  EXPECT_EQ(round_trip.user, "alice smith");
  EXPECT_EQ(round_trip.parameter("note"), "a;b");
}

// ---------------------------------------------------------------- 19.1.4 comparison

// RFC 3261 19.1.4: the user part is case-sensitive, the host is not.
TEST(SIPUriRFCTest, HostIsCaseInsensitiveAndUserIsNot) {
  EXPECT_TRUE(SIPUri("sip:alice@example.com").equivalent_to(SIPUri("sip:alice@EXAMPLE.COM")));
  EXPECT_FALSE(SIPUri("sip:alice@example.com").equivalent_to(SIPUri("sip:ALICE@example.com")));
}

// RFC 3261 19.1.4: a missing port is not equivalent to the default port.
TEST(SIPUriRFCTest, AnAbsentPortIsNotTheDefaultPort) {
  EXPECT_FALSE(SIPUri("sip:alice@example.com").equivalent_to(SIPUri("sip:alice@example.com:5060")));
}

// RFC 3261 19.1.4: a parameter in both must match; one present in only one side is
// ignored, except for the four named below.
TEST(SIPUriRFCTest, AParameterInOnlyOneUriIsIgnored) {
  EXPECT_TRUE(SIPUri("sip:alice@example.com;transport=tcp").equivalent_to(SIPUri("sip:alice@example.com;transport=tcp;other=1")));
}

TEST(SIPUriRFCTest, AParameterPresentInBothMustMatch) {
  EXPECT_FALSE(SIPUri("sip:alice@example.com;transport=tcp").equivalent_to(SIPUri("sip:alice@example.com;transport=udp")));
}

// RFC 3261 19.1.4: a user, ttl, method or maddr parameter in only one URI never matches.
TEST(SIPUriRFCTest, UserTtlMethodAndMaddrNeverMatchWhenOnlyOneSideHasThem) {
  EXPECT_FALSE(SIPUri("sip:alice@example.com;user=phone").equivalent_to(SIPUri("sip:alice@example.com")));
  EXPECT_FALSE(SIPUri("sip:alice@example.com;ttl=15").equivalent_to(SIPUri("sip:alice@example.com")));
  EXPECT_FALSE(SIPUri("sip:alice@example.com;method=INVITE").equivalent_to(SIPUri("sip:alice@example.com")));
  EXPECT_FALSE(SIPUri("sip:alice@example.com;maddr=239.255.255.1").equivalent_to(SIPUri("sip:alice@example.com")));
}

// RFC 3261 19.1.4: a header component must be present in both URIs and match.
TEST(SIPUriRFCTest, HeadersMustBePresentInBothAndMatch) {
  EXPECT_FALSE(SIPUri("sip:alice@example.com?Subject=x").equivalent_to(SIPUri("sip:alice@example.com")));
  EXPECT_TRUE(SIPUri("sip:alice@example.com?Subject=x").equivalent_to(SIPUri("sip:alice@example.com?Subject=x")));
}

// RFC 3261 19.1.4: sip and sips are different schemes and never equivalent.
TEST(SIPUriRFCTest, SipAndSipsAreNotEquivalent) {
  EXPECT_FALSE(SIPUri("sip:alice@example.com").equivalent_to(SIPUri("sips:alice@example.com")));
}

// Escaped and unescaped forms are equivalent, or a REGISTER's Contact would create a second binding for the
// same contact.
TEST(SIPUriRFCTest, EscapingDoesNotChangeEquivalence) {
  EXPECT_TRUE(SIPUri("sip:alice%20smith@example.com").equivalent_to(SIPUri("sip:alice smith@example.com")));
}

// ----------------------------------------------------------------------- host naming

// The field is the URI's host (19.1.1), not a realm, which is the Digest protection domain.
TEST(SIPUriRFCTest, TheHostFieldIsCalledHost) {
  SIPUri uri("sip:alice@example.com:5060");

  ASSERT_TRUE(uri.valid);
  EXPECT_EQ(uri.host, "example.com");
}
