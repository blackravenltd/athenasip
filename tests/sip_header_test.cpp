//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_header.h"

#include <gtest/gtest.h>

#include "headers/via_header.h"

using namespace athenasip;
using athenasip::headers::ViaHeader;

namespace {

const char* kRegister =
    "REGISTER sip:example.com SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-one\r\n"
    "From: <sip:alice@example.com>;tag=abc\r\n"
    "To: <sip:alice@example.com>\r\n"
    "Call-ID: call-1234\r\n"
    "CSeq: 1 REGISTER\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

}  // namespace

TEST(SIPHeaderTest, ParsesRequestLine) {
  SIPHeader header(kRegister);

  EXPECT_EQ(header.type, SIPHeader::Type::Request);
  EXPECT_EQ(header.request_method, "REGISTER");
  EXPECT_EQ(header.sip_version, "SIP/2.0");
  EXPECT_EQ(header.first_line(), "REGISTER sip:example.com SIP/2.0");
}

TEST(SIPHeaderTest, ParsesResponseLine) {
  SIPHeader header(
      "SIP/2.0 401 Unauthorized\r\n"
      "Call-ID: call-1234\r\n"
      "\r\n");

  EXPECT_EQ(header.type, SIPHeader::Type::Response);
  EXPECT_EQ(header.response_code, 401);
  EXPECT_EQ(header.response_message, "Unauthorized");
  EXPECT_EQ(header.first_line(), "SIP/2.0 401 Unauthorized");
}

TEST(SIPHeaderTest, ParsesHeadersInOrder) {
  SIPHeader header(kRegister);

  ASSERT_EQ(header.headers.size(), 6u);
  EXPECT_EQ(header.headers[0].key, "Via");
  EXPECT_EQ(header.headers[1].key, "From");
  EXPECT_EQ(header.headers[5].key, "Content-Length");

  EXPECT_TRUE(header.contains("Call-ID"));
  EXPECT_FALSE(header.contains("Authorization"));
}

TEST(SIPHeaderTest, FoldedContinuationLinesAreJoined) {
  SIPHeader header(
      "REGISTER sip:example.com SIP/2.0\r\n"
      "Subject: the first part\r\n"
      "  and the continuation\r\n"
      "Call-ID: call-1234\r\n"
      "\r\n");

  ASSERT_TRUE(header.contains("Subject"));
  EXPECT_EQ(header.headers_map["Subject"][0]->to_string(), "the first part and the continuation");
}

TEST(SIPHeaderTest, AddAppends) {
  SIPHeader header;
  header.type = SIPHeader::Type::Request;

  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP first:5060;branch=one"));
  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP second:5060;branch=two"));

  ASSERT_EQ(header.headers_map["Via"].size(), 2u);
  EXPECT_EQ(header.headers_map["Via"][0]->as<ViaHeader>()->host, "first:5060");
  EXPECT_EQ(header.headers_map["Via"][1]->as<ViaHeader>()->host, "second:5060");
}

// RFC 3261 16.6 step 8: a proxy prepends its Via. headers_map order must match the
// vector, since lookups go through the map.
TEST(SIPHeaderTest, AddStartPrependsInBothOrderings) {
  SIPHeader header;
  header.type = SIPHeader::Type::Request;

  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP downstream:5060;branch=old"));
  header.add_start("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP proxy:5060;branch=new"));

  ASSERT_EQ(header.headers.size(), 2u);
  EXPECT_EQ(header.headers[0].value->as<ViaHeader>()->host, "proxy:5060");
  EXPECT_EQ(header.headers[1].value->as<ViaHeader>()->host, "downstream:5060");

  ASSERT_EQ(header.headers_map["Via"].size(), 2u);
  EXPECT_EQ(header.headers_map["Via"][0]->as<ViaHeader>()->host, "proxy:5060");
  EXPECT_EQ(header.headers_map["Via"][1]->as<ViaHeader>()->host, "downstream:5060");
}

TEST(SIPHeaderTest, AddStartOnEmptyFieldStillRegisters) {
  SIPHeader header;
  header.type = SIPHeader::Type::Request;

  header.add_start("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP only:5060;branch=one"));

  ASSERT_EQ(header.headers_map["Via"].size(), 1u);
  EXPECT_EQ(header.headers_map["Via"][0]->as<ViaHeader>()->host, "only:5060");
}

TEST(SIPHeaderTest, ClearRemovesEveryInstance) {
  SIPHeader header(kRegister);

  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP second:5060;branch=two"));
  header.clear("Via");

  EXPECT_FALSE(header.contains("Via"));
  for (const auto& field : header.headers) EXPECT_NE(field.key, "Via");
}

TEST(SIPHeaderTest, RemoveValueRemovesOnlyMatchesAndRebuildsTheMap) {
  SIPHeader header;
  header.type = SIPHeader::Type::Request;

  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP keep:5060;branch=one"));
  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP drop:5060;branch=two"));
  header.add("Via", std::make_shared<ViaHeader>("SIP/2.0/UDP keep:5061;branch=three"));

  header.remove_value("Via", [](std::shared_ptr<athenasip::headers::Header> h) { return h->as<ViaHeader>()->host == "drop:5060"; });

  ASSERT_EQ(header.headers_map["Via"].size(), 2u);
  EXPECT_EQ(header.headers_map["Via"][0]->as<ViaHeader>()->host, "keep:5060");
  EXPECT_EQ(header.headers_map["Via"][1]->as<ViaHeader>()->host, "keep:5061");
  EXPECT_EQ(header.headers.size(), 2u);
}

TEST(SIPHeaderTest, ToStringRoundTripsFirstLineAndHeaders) {
  SIPHeader header(kRegister);
  const auto out = header.to_string();

  EXPECT_EQ(out.rfind("REGISTER sip:example.com SIP/2.0\r\n", 0), 0u);
  EXPECT_NE(out.find("Call-ID: call-1234\r\n"), std::string::npos);
  EXPECT_NE(out.find("CSeq: 1 REGISTER\r\n"), std::string::npos);
}

// A default-constructed header is a Request with a null request_uri, so first_line()
// throws rather than dereferencing it.
TEST(SIPHeaderTest, FirstLineThrowsWhenRequestUriIsMissing) {
  SIPHeader header;
  header.request_method = "INVITE";

  EXPECT_THROW(header.first_line(), std::runtime_error);
}

TEST(SIPHeaderTest, FirstLineSerialisesOnceRequestUriIsSet) {
  SIPHeader header;
  header.request_method = "INVITE";
  header.request_uri = std::make_shared<SIPUri>("sip:bob@example.com");

  EXPECT_EQ(header.first_line(), "INVITE sip:bob@example.com SIP/2.0");
}
