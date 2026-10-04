//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "headers/sip_identity_header.h"
#include "headers/via_header.h"
#include "sip_message.h"

using namespace athenasip;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::ViaHeader;

namespace {

std::shared_ptr<SIPMessage> request_with(const std::string& text) {
  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(text);
  return message;
}

// A request that crossed two proxies, so it carries three Via values.
const char* kProxiedInvite =
    "INVITE sip:bob@example.com SIP/2.0\r\n"
    "Via: SIP/2.0/UDP proxy2.example.com:5060;branch=z9hG4bK-three\r\n"
    "Via: SIP/2.0/UDP proxy1.example.com:5060;branch=z9hG4bK-two\r\n"
    "Via: SIP/2.0/UDP alice-phone.example.com:5060;branch=z9hG4bK-one\r\n"
    "From: \"Alice\" <sip:alice@example.com>;tag=alice-tag\r\n"
    "To: <sip:bob@example.com>\r\n"
    "Call-ID: call-abcdef\r\n"
    "CSeq: 314159 INVITE\r\n"
    "\r\n";

}  // namespace

// RFC 3261 8.2.6.2: the response copies every Via from the request, in order.
TEST(SIPMessageTest, ResponseCopiesEveryViaInOrder) {
  auto request = request_with(kProxiedInvite);
  ASSERT_EQ(request->header->headers_map["Via"].size(), 3u);

  auto response = request->generate_response();

  ASSERT_EQ(response->header->headers_map["Via"].size(), 3u);
  EXPECT_EQ(response->header->headers_map["Via"][0]->as<ViaHeader>()->host, "proxy2.example.com:5060");
  EXPECT_EQ(response->header->headers_map["Via"][1]->as<ViaHeader>()->host, "proxy1.example.com:5060");
  EXPECT_EQ(response->header->headers_map["Via"][2]->as<ViaHeader>()->host, "alice-phone.example.com:5060");
}

// RFC 3261 8.2.6.2: From, Call-ID and CSeq are copied unchanged.
TEST(SIPMessageTest, ResponseCopiesFromCallIdAndCSeq) {
  auto response = request_with(kProxiedInvite)->generate_response();

  EXPECT_EQ(response->header->headers_map["Call-ID"][0]->to_string(), "call-abcdef");
  EXPECT_EQ(response->header->headers_map["CSeq"][0]->to_string(), "314159 INVITE");
  EXPECT_NE(response->header->headers_map["From"][0]->to_string().find("alice@example.com"), std::string::npos);
  EXPECT_NE(response->header->headers_map["From"][0]->to_string().find("tag=alice-tag"), std::string::npos);
}

// RFC 3261 8.2.6.2: a To without a tag gets one in the response. The request is not
// modified, since it is still needed for transaction matching.
TEST(SIPMessageTest, ResponseAddsToTagWithoutMutatingTheRequest) {
  auto request = request_with(kProxiedInvite);
  const auto to_before = request->header->headers_map["To"][0]->to_string();
  EXPECT_EQ(to_before.find("tag="), std::string::npos);

  auto response = request->generate_response();

  EXPECT_NE(response->header->headers_map["To"][0]->to_string().find("tag="), std::string::npos);
  EXPECT_EQ(request->header->headers_map["To"][0]->to_string(), to_before);
}

TEST(SIPMessageTest, ResponseIsAResponse) {
  auto response = request_with(kProxiedInvite)->generate_response();
  EXPECT_EQ(response->header->type, SIPHeader::Type::Response);
}

// RFC 3261 17.1.3, 17.2.3: a transaction is matched on the top Via's branch and sent-by
// and the method. Branch alone collides across upstream hops.
TEST(SIPMessageTest, TransactionIdIncludesSentBy) {
  auto from_proxy_one = request_with(
      "INVITE sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP proxy1.example.com:5060;branch=z9hG4bK-same\r\n"
      "CSeq: 1 INVITE\r\n"
      "\r\n");

  auto from_proxy_two = request_with(
      "INVITE sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP proxy2.example.com:5060;branch=z9hG4bK-same\r\n"
      "CSeq: 1 INVITE\r\n"
      "\r\n");

  EXPECT_NE(from_proxy_one->get_transaction_id(), from_proxy_two->get_transaction_id());
}

TEST(SIPMessageTest, TransactionIdIsStableForTheSameRequest) {
  auto a = request_with(kProxiedInvite);
  auto b = request_with(kProxiedInvite);

  EXPECT_EQ(a->get_transaction_id(), b->get_transaction_id());
}

TEST(SIPMessageTest, ToStringSeparatesHeadersFromBodyWithABlankLine) {
  auto message = request_with(kProxiedInvite);
  message->body = "v=0\r\n";

  const auto text = message->to_string();
  EXPECT_NE(text.find("\r\n\r\nv=0"), std::string::npos);
}
