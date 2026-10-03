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

// RFC 5389 section 6: a Binding request with no attributes, the shape RFC 5626 4.4.2
// keep-alives take.
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

// Only a STUN message is one: the top two bits clear, the magic cookie in place and a length
// that matches. SIP starts with a letter, so the two cannot be confused (RFC 5626 4.4.2,
// RFC 5389 section 6).
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

// RFC 5389 section 7.3.1: the answer to a Binding request is a success response with the
// same transaction id and the source address in XOR-MAPPED-ADDRESS (section 15.2), which is
// how a client behind a NAT learns its mapping has moved.
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

// IPv6 XORs the address with the cookie and the transaction id together (section 15.2).
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

// Anything but a Binding request - a response, an indication, another method - is not
// answered: answering a response would be two servers talking to each other forever.
TEST(StunTest, OnlyABindingRequestIsAnswered) {
  auto response = binding_request();
  response[0] = '\x01';
  response[1] = '\x01';
  EXPECT_FALSE(stun::binding_response(response, boost::asio::ip::make_address("192.0.2.10"), 5060).has_value());

  auto indication = binding_request();
  indication[1] = '\x11';
  EXPECT_FALSE(stun::binding_response(indication, boost::asio::ip::make_address("192.0.2.10"), 5060).has_value());
}
