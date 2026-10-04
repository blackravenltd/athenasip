//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "headers/via_header.h"
#include "sip_header.h"

using namespace athenasip;
using athenasip::headers::ViaHeader;

// RFC 3261 7.3.1: field names are case-insensitive.
TEST(SIPHeaderRFCTest, FieldNamesAreCaseInsensitive) {
  SIPHeader header(
      "REGISTER sip:example.com SIP/2.0\r\n"
      "call-id: call-1234\r\n"
      "CONTENT-LENGTH: 0\r\n"
      "\r\n");

  EXPECT_TRUE(header.contains("Call-ID"));
  EXPECT_TRUE(header.contains("Content-Length"));
}

// RFC 3261 7.3.3, 20: compact forms are equivalent to the long names.
TEST(SIPHeaderRFCTest, CompactFormsResolveToLongNames) {
  SIPHeader header(
      "REGISTER sip:example.com SIP/2.0\r\n"
      "v: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-one\r\n"
      "i: call-1234\r\n"
      "l: 0\r\n"
      "\r\n");

  EXPECT_TRUE(header.contains("Via"));
  EXPECT_TRUE(header.contains("Call-ID"));
  EXPECT_TRUE(header.contains("Content-Length"));
}

// RFC 3261 7.3.1: a comma-separated list in one row is equivalent to separate rows.
TEST(SIPHeaderRFCTest, CommaSeparatedValuesSplitIntoSeparateHeaders) {
  SIPHeader header(
      "REGISTER sip:example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP first:5060;branch=one, SIP/2.0/UDP second:5060;branch=two\r\n"
      "\r\n");

  ASSERT_EQ(header.headers_map["Via"].size(), 2u);
  EXPECT_EQ(header.headers_map["Via"][0]->as<ViaHeader>()->host, "first:5060");
  EXPECT_EQ(header.headers_map["Via"][1]->as<ViaHeader>()->host, "second:5060");
}

// RFC 3261 8.2.1, 16.3: an unparseable request is answered 400, so a failed parse is
// reported as invalid.
TEST(SIPHeaderRFCTest, MalformedStartLineIsReportedInvalid) {
  EXPECT_FALSE(SIPHeader("NOT A SIP MESSAGE AT ALL\r\n\r\n").is_valid());
  EXPECT_FALSE(SIPHeader("REGISTER\r\n\r\n").is_valid());
  EXPECT_FALSE(SIPHeader("REGISTER sip:example.com HTTP/1.1\r\n\r\n").is_valid());
  EXPECT_FALSE(SIPHeader("\r\n").is_valid());
}

TEST(SIPHeaderRFCTest, MalformedStatusLineIsReportedInvalid) {
  EXPECT_FALSE(SIPHeader("SIP/2.0\r\n\r\n").is_valid());
  EXPECT_FALSE(SIPHeader("SIP/2.0 99 Too Small\r\n\r\n").is_valid());
  EXPECT_FALSE(SIPHeader("SIP/2.0 700 Too Big\r\n\r\n").is_valid());
}

TEST(SIPHeaderRFCTest, HeaderLineWithoutColonIsReportedInvalid) {
  SIPHeader header(
      "REGISTER sip:example.com SIP/2.0\r\n"
      "this line has no colon\r\n"
      "\r\n");

  EXPECT_FALSE(header.is_valid());
}

TEST(SIPHeaderRFCTest, WellFormedMessagesAreValid) {
  EXPECT_TRUE(SIPHeader("REGISTER sip:example.com SIP/2.0\r\n"
                        "Call-ID: call-1234\r\n"
                        "\r\n")
                  .is_valid());

  EXPECT_TRUE(SIPHeader("SIP/2.0 401 Unauthorized\r\n"
                        "Call-ID: call-1234\r\n"
                        "\r\n")
                  .is_valid());
}
