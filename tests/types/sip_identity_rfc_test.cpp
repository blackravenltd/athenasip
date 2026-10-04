//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/sip_identity.h"

using athenasip::types::SIPIdentity;

// RFC 3261 20.10 (Contact), 20.20 (From) and 25.1:
//
//   contact-param = (name-addr / addr-spec) *(SEMI contact-params)
//   name-addr     = [ display-name ] LAQUOT addr-spec RAQUOT
//   display-name  = *(token LWS) / quoted-string

// ------------------------------------------------------------------- display-name

// A quoted-string display name may contain the characters that delimit everything else.
TEST(SIPIdentityRFCTest, AQuotedDisplayNameMayContainAngleBracketsAndSemicolons) {
  SIPIdentity identity("\"Alice <the boss>;odd\" <sip:alice@example.com>;tag=x");

  ASSERT_NE(identity.uri, nullptr);
  ASSERT_TRUE(identity.display_name.has_value());
  EXPECT_EQ(identity.display_name.value(), "Alice <the boss>;odd");
  EXPECT_EQ(identity.uri->user, "alice");
  EXPECT_EQ(identity.tags["tag"], "x");
}

// RFC 3261 25.1: quoted-pair = "\" (any). A quote inside a quoted string is escaped,
// and the escape is not part of the value.
TEST(SIPIdentityRFCTest, AQuotedDisplayNameMayContainEscapedQuotes) {
  SIPIdentity identity("\"Alice \\\"Al\\\" Smith\" <sip:alice@example.com>");

  ASSERT_NE(identity.uri, nullptr);
  ASSERT_TRUE(identity.display_name.has_value());
  EXPECT_EQ(identity.display_name.value(), "Alice \"Al\" Smith");
}

// display-name = *(token LWS): an unquoted run of tokens is a display name too.
TEST(SIPIdentityRFCTest, AnUnquotedDisplayNameIsASequenceOfTokens) {
  SIPIdentity identity("Alice Smith <sip:alice@example.com>;tag=x");

  ASSERT_NE(identity.uri, nullptr);
  ASSERT_TRUE(identity.display_name.has_value());
  EXPECT_EQ(identity.display_name.value(), "Alice Smith");
  EXPECT_EQ(identity.uri->user, "alice");
}

TEST(SIPIdentityRFCTest, NoDisplayNameLeavesItUnset) {
  SIPIdentity identity("<sip:alice@example.com>;tag=x");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_FALSE(identity.display_name.has_value());
}

// --------------------------------------------------------------------- parameters

// RFC 3261 7.3.1: header field parameter names are case-insensitive.
TEST(SIPIdentityRFCTest, ParameterNamesAreCaseInsensitive) {
  SIPIdentity identity("<sip:alice@example.com>;Tag=abc;EXPIRES=60");

  EXPECT_EQ(identity.tags["tag"], "abc");
  EXPECT_EQ(identity.tags["expires"], "60");
}

// RFC 3261 25.1: EQUAL = SWS "=" SWS, so whitespace around the '=' and the ';' is
// legal and carries no meaning.
TEST(SIPIdentityRFCTest, WhitespaceAroundParameterDelimitersIsIgnored) {
  SIPIdentity identity("  \"Alice\"  <sip:alice@example.com> ; tag = abc ; lr ");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.tags["tag"], "abc");
  EXPECT_TRUE(identity.tags.contains("lr"));
  EXPECT_EQ(identity.tags["lr"], "");
}

// RFC 3261 20: with no angle brackets the first semicolon starts the header
// parameters, so they are not part of the URI.
TEST(SIPIdentityRFCTest, AddrSpecParametersBelongToTheHeader) {
  SIPIdentity identity("sip:alice@example.com;tag=abc");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_TRUE(identity.uri->parameters().empty());
  EXPECT_EQ(identity.tags["tag"], "abc");
}

// And inside the brackets the same semicolon is a URI parameter.
TEST(SIPIdentityRFCTest, NameAddrParametersInsideTheBracketsBelongToTheUri) {
  SIPIdentity identity("<sip:alice@example.com;transport=tcp>;tag=abc");

  ASSERT_NE(identity.uri, nullptr);
  EXPECT_EQ(identity.uri->parameter("transport"), "tcp");
  EXPECT_EQ(identity.tags["tag"], "abc");
  EXPECT_EQ(identity.tags.count("transport"), 0u);
}

// --------------------------------------------------------------------- STAR

// RFC 3261 20.10: a lone "*" is a Contact alternative, not a URI. With Expires 0 it removes every binding (10.2.2).
TEST(SIPIdentityRFCTest, AStarContactIsRecognisedAsStar) {
  SIPIdentity identity("*");

  EXPECT_TRUE(identity.star);
  EXPECT_EQ(identity.uri, nullptr);
  EXPECT_FALSE(identity.display_name.has_value());
}

TEST(SIPIdentityRFCTest, AStarContactToleratesSurroundingWhitespace) {
  SIPIdentity identity("  *  ");

  EXPECT_TRUE(identity.star);
}

// A URI is not a star, and a star is not a URI whose host happens to be "*".
TEST(SIPIdentityRFCTest, AnOrdinaryContactIsNotStar) {
  SIPIdentity identity("<sip:alice@example.com>");

  EXPECT_FALSE(identity.star);
  ASSERT_NE(identity.uri, nullptr);
}

TEST(SIPIdentityRFCTest, StarSerialisesBackToStar) {
  SIPIdentity identity("*");

  EXPECT_EQ(identity.to_string(), "*");
}

// ------------------------------------------------------------------ serialisation

// The same identity must serialise to the same bytes, or a retransmission does not match what was sent.
TEST(SIPIdentityRFCTest, SerialisationIsDeterministic) {
  SIPIdentity identity("<sip:alice@example.com>;tag=abc;expires=60;q=0.5");

  const auto first = identity.to_string();
  for (int i = 0; i < 8; ++i) {
    SIPIdentity again(first);
    EXPECT_EQ(again.to_string(), first);
  }
}

TEST(SIPIdentityRFCTest, RoundTripsAQuotedDisplayName) {
  SIPIdentity identity("\"Alice <the boss>\" <sip:alice@example.com>;tag=x");

  SIPIdentity again(identity.to_string());
  ASSERT_TRUE(again.display_name.has_value());
  EXPECT_EQ(again.display_name.value(), "Alice <the boss>");
  EXPECT_EQ(again.tags["tag"], "x");
}
