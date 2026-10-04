//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "stun.h"

#include <gtest/gtest.h>

#include <boost/asio/ip/address.hpp>
#include <string>

using namespace athenasip;

namespace {

// RFC 5389 6: a Binding request with no attributes, as RFC 5626 4.4.2 keep-alives are.
std::string binding_request(const std::string& transaction = "ABCDEFGHIJKL") {
  std::string raw;
  raw += '\x00';
  raw += '\x01';  // Binding request
  raw += '\x00';
  raw += '\x00';  // no attributes
  raw += '\x21';
  raw += '\x12';
  raw += '\xA4';
  raw += '\x42';  // the magic cookie
  raw += transaction;
  return raw;
}

std::uint16_t u16(const std::string& s, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]));
}

}  // namespace

// RFC 5389 6, RFC 5626 4.4.2: a STUN message has its top two bits clear, the magic
// cookie in place and a matching length, so it cannot be confused with SIP.
TEST(StunTest, OnlyAStunMessageIsRecognised) {
  EXPECT_TRUE(stun::is_stun(binding_request()));

  EXPECT_FALSE(stun::is_stun("REGISTER sip:example.com SIP/2.0\r\n\r\n"));
  EXPECT_FALSE(stun::is_stun("\r\n\r\n"));
  EXPECT_FALSE(stun::is_stun(binding_request().substr(0, 19)));

  auto wrong_cookie = binding_request();
  wrong_cookie[4] = '\x00';
  EXPECT_FALSE(stun::is_stun(wrong_cookie));

  auto wrong_length = binding_request();
  wrong_length[3] = '\x04';
  EXPECT_FALSE(stun::is_stun(wrong_length));
}

// RFC 5389 7.3.1, 15.2: a Binding request is answered with a success response carrying
// the same transaction id and the source address in XOR-MAPPED-ADDRESS.
TEST(StunTest, ABindingRequestIsAnsweredWithWhereItCameFrom) {
  const auto response = stun::binding_response(binding_request("0123456789AB"), boost::asio::ip::make_address("192.0.2.10"), 5062);
  ASSERT_TRUE(response.has_value());

  ASSERT_EQ(response->size(), 32u);
  EXPECT_EQ(u16(*response, 0), 0x0101) << "Binding success response";
  EXPECT_EQ(u16(*response, 2), 12) << "one attribute of 8 bytes and its 4 byte header";
  EXPECT_EQ(response->substr(4, 4), std::string("\x21\x12\xA4\x42", 4));
  EXPECT_EQ(response->substr(8, 12), "0123456789AB");

  EXPECT_EQ(u16(*response, 20), 0x0020) << "XOR-MAPPED-ADDRESS";
  EXPECT_EQ(u16(*response, 22), 8);
  EXPECT_EQ(static_cast<unsigned char>((*response)[25]), 0x01) << "IPv4";
  EXPECT_EQ(u16(*response, 26) ^ 0x2112, 5062);

  const unsigned char cookie[4] = {0x21, 0x12, 0xA4, 0x42};
  const unsigned char expected[4] = {192, 0, 2, 10};
  for (int i = 0; i < 4; ++i) EXPECT_EQ(static_cast<unsigned char>((*response)[28 + i]) ^ cookie[i], expected[i]);
}

// RFC 5389 15.2: an IPv6 address is XORed with the cookie and the transaction id.
TEST(StunTest, AnIpv6SourceIsXoredWithTheCookieAndTheTransaction) {
  const std::string transaction = "0123456789AB";
  const auto source = boost::asio::ip::make_address("2001:db8::1");
  const auto response = stun::binding_response(binding_request(transaction), source, 5060);
  ASSERT_TRUE(response.has_value());
  ASSERT_EQ(response->size(), 44u);

  EXPECT_EQ(static_cast<unsigned char>((*response)[25]), 0x02);

  const auto bytes = source.to_v6().to_bytes();
  const std::string mask = std::string("\x21\x12\xA4\x42", 4) + transaction;
  for (int i = 0; i < 16; ++i) EXPECT_EQ(static_cast<unsigned char>((*response)[28 + i]) ^ static_cast<unsigned char>(mask[i]), bytes[i]);
}

// Only a Binding request is answered; answering a response would loop between servers.
TEST(StunTest, OnlyABindingRequestIsAnswered) {
  auto response = binding_request();
  response[0] = '\x01';
  response[1] = '\x01';
  EXPECT_FALSE(stun::binding_response(response, boost::asio::ip::make_address("192.0.2.10"), 5060).has_value());

  auto indication = binding_request();
  indication[1] = '\x11';
  EXPECT_FALSE(stun::binding_response(indication, boost::asio::ip::make_address("192.0.2.10"), 5060).has_value());
}

namespace {

std::string from_hex(const std::string& hex) {
  std::string out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
  return out;
}

// RFC 5769 2.2: the sample IPv4 Binding success response, whose mapped address is 192.0.2.1 port 32853.
std::string sample_response() {
  return from_hex(
      "0101003c2112a442b7e7a701bc34d686fa87dfae"  // header and transaction id
      "8022000b74657374207665637"
      "46f7220"  // SOFTWARE "test vector", padded
      "0020000800"
      "01a147e112a643"                                    // XOR-MAPPED-ADDRESS
      "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"  // MESSAGE-INTEGRITY
      "80280004c07d4c96");                                // FINGERPRINT
}

}  // namespace

// RFC 5389 6: a Binding request this node sends is a header with the cookie and its transaction id, and no
// attributes; it is recognised as STUN.
TEST(StunTest, ABindingRequestCarriesItsTransactionId) {
  const auto request = stun::binding_request("ABCDEFGHIJKL");

  ASSERT_EQ(request.size(), 20u);
  EXPECT_EQ(request.substr(0, 4), std::string("\x00\x01\x00\x00", 4));
  EXPECT_EQ(request.substr(4, 4), std::string("\x21\x12\xA4\x42", 4));
  EXPECT_EQ(request.substr(8), "ABCDEFGHIJKL");
  EXPECT_TRUE(stun::is_stun(request));
}

// RFC 5769 2.2: the mapped address of the sample response, read from XOR-MAPPED-ADDRESS (RFC 5389 15.2).
TEST(StunTest, TheSampleResponseMapsTo192_0_2_1Port32853) {
  const auto mapped = stun::binding_success(sample_response());

  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(mapped->address.to_string(), "192.0.2.1");
  EXPECT_EQ(mapped->port, 32853);
  EXPECT_EQ(mapped->transaction_id, from_hex("b7e7a701bc34d686fa87dfae"));
}

// What this node answers a keep-alive with is read back to the address it named.
TEST(StunTest, ThisNodesOwnResponseReadsBack) {
  const auto response = stun::binding_response(binding_request(), boost::asio::ip::make_address("203.0.113.7"), 40000);
  ASSERT_TRUE(response.has_value());

  const auto mapped = stun::binding_success(*response);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(mapped->address.to_string(), "203.0.113.7");
  EXPECT_EQ(mapped->port, 40000);
}

// A request is not a success response.
TEST(StunTest, ARequestIsNotASuccessResponse) { EXPECT_FALSE(stun::binding_success(binding_request()).has_value()); }
