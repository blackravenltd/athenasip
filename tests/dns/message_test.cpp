//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/message.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace athenasip::dns;

namespace {

std::vector<std::uint8_t> from_hex(const std::string& hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

// Real responses from 1.1.1.1 on 2026-10-01, byte for byte. The expectations beside each
// were read off the bytes by hand from RFC 1035, 2782 and 3403, not produced by the code
// under test.
const char* kSip2SipNaptr =
    "4ff981800001000300000000077369703273697004696e666f0000230001c00c0023000100000e100027000a00640173075349502b44325400045f736970045f74637007"
    "7369703273697004696e666f00c00c002300010000012c0027001400640173075349502b44325500045f736970045f756470077369703273697004696e666f00c00c00"
    "23000100000e10002900190064017308534950532b44325400055f73697073045f746370077369703273697004696e666f00";

const char* kIptelSrv =
    "bc1181800001000100000000045f736970045f75647005697074656c036f72670000210001c00c002100010000025800150000001913c40373697005697074656c036f726700";

const char* kSip2SipSipsSrv =
    "0bd181800001000100000000055f73697073045f746370077369703273697004696e666f0000210001c00c002100010000012c00190064006413c50570726f7879077369"
    "7074686f72036e657400";

const char* kIptelA = "c2478180000100010000000005697074656c036f72670000010001c00c00010001000002580004d44f6f9b";

const char* kGoogleAaaa =
    "0af38180000100040000000006676f6f676c6503636f6d00001c0001c00c001c00010000002200102a00145040100c0f0000000000000071c00c001c00010000002200102a"
    "00145040100c0f0000000000000066c00c001c00010000002200102a00145040100c0f000000000000008ac00c001c00010000002200102a00145040100c0f00000000"
    "00000064";

// No answer, one SOA in the authority section: what a server says when the name exists and
// has nothing of the type asked for.
const char* kIptelNoNaptr =
    "b4ce8180000100000001000005697074656c036f72670000230001c00c000600010000012c00310178026e73056a6f6b657203636f6d000a686f73746d6173746572c02c"
    "78c3b2770000384000000e10001275000000012c";

}  // namespace

// RFC 1035 4.1.1 and 4.1.2: a 12-byte header with the id, RD set and one question; the
// name as length-prefixed labels ending in a zero; then QTYPE and QCLASS IN.
TEST(DnsMessageTest, AQueryIsTheHeaderTheLabelsAndTheTypeAndClass) {
  const auto query = encode_query(0xbc11, "_sip._udp.iptel.org", Type::SRV);

  // id, flags (RD), QDCOUNT 1, AN/NS/ARCOUNT 0; then the question as the server echoed it.
  const auto expected = from_hex(
      "bc11"
      "0100"
      "0001"
      "0000"
      "0000"
      "0000"
      "045f736970045f75647005697074656c036f72670000210001");
  EXPECT_EQ(query, expected);
}

// A trailing dot is the root, already implied: it does not add an empty label.
TEST(DnsMessageTest, AFullyQualifiedNameEncodesTheSame) { EXPECT_EQ(encode_query(1, "iptel.org.", Type::A), encode_query(1, "iptel.org", Type::A)); }

// RFC 3403 section 4.1: order, preference, then three character-strings and a domain name
// that is never compressed. Three of them here, in the order the server sent them.
TEST(DnsMessageTest, NaptrRecordsAreReadInFull) {
  const auto response = decode_response(from_hex(kSip2SipNaptr));
  ASSERT_TRUE(response.has_value());

  EXPECT_EQ(response->id, 0x4ff9);
  EXPECT_EQ(response->rcode, 0);
  ASSERT_EQ(response->answers.size(), 3u);

  const auto& tcp = response->answers[0];
  EXPECT_EQ(tcp.type, Type::NAPTR);
  EXPECT_EQ(tcp.name, "sip2sip.info");
  EXPECT_EQ(tcp.ttl, 3600u);
  EXPECT_EQ(tcp.naptr.order, 10);
  EXPECT_EQ(tcp.naptr.preference, 100);
  EXPECT_EQ(tcp.naptr.flags, "s");
  EXPECT_EQ(tcp.naptr.services, "SIP+D2T");
  EXPECT_EQ(tcp.naptr.regexp, "");
  EXPECT_EQ(tcp.naptr.replacement, "_sip._tcp.sip2sip.info");

  EXPECT_EQ(response->answers[1].ttl, 300u);
  EXPECT_EQ(response->answers[1].naptr.order, 20);
  EXPECT_EQ(response->answers[1].naptr.services, "SIP+D2U");
  EXPECT_EQ(response->answers[1].naptr.replacement, "_sip._udp.sip2sip.info");

  EXPECT_EQ(response->answers[2].naptr.order, 25);
  EXPECT_EQ(response->answers[2].naptr.services, "SIPS+D2T");
  EXPECT_EQ(response->answers[2].naptr.replacement, "_sips._tcp.sip2sip.info");
}

// RFC 2782: priority, weight, port, target. The owner name is a compression pointer back
// to the question (RFC 1035 4.1.4), which is where nearly every answer's name comes from.
TEST(DnsMessageTest, SrvRecordsAreReadAndTheirOwnerNameIsDecompressed) {
  const auto response = decode_response(from_hex(kIptelSrv));
  ASSERT_TRUE(response.has_value());
  ASSERT_EQ(response->answers.size(), 1u);

  const auto& srv = response->answers[0];
  EXPECT_EQ(srv.type, Type::SRV);
  EXPECT_EQ(srv.name, "_sip._udp.iptel.org");
  EXPECT_EQ(srv.ttl, 600u);
  EXPECT_EQ(srv.srv.priority, 0);
  EXPECT_EQ(srv.srv.weight, 25);
  EXPECT_EQ(srv.srv.port, 5060);
  EXPECT_EQ(srv.srv.target, "sip.iptel.org");
}

TEST(DnsMessageTest, AnSrvForSipsCarriesItsOwnPort) {
  const auto response = decode_response(from_hex(kSip2SipSipsSrv));
  ASSERT_TRUE(response.has_value());
  ASSERT_EQ(response->answers.size(), 1u);

  EXPECT_EQ(response->answers[0].srv.priority, 100);
  EXPECT_EQ(response->answers[0].srv.weight, 100);
  EXPECT_EQ(response->answers[0].srv.port, 5061);
  EXPECT_EQ(response->answers[0].srv.target, "proxy.sipthor.net");
}

TEST(DnsMessageTest, AnARecordIsTheDottedQuad) {
  const auto response = decode_response(from_hex(kIptelA));
  ASSERT_TRUE(response.has_value());
  ASSERT_EQ(response->answers.size(), 1u);

  EXPECT_EQ(response->answers[0].type, Type::A);
  EXPECT_EQ(response->answers[0].address, "212.79.111.155");
}

TEST(DnsMessageTest, AnAaaaRecordIsTheCompressedTextForm) {
  const auto response = decode_response(from_hex(kGoogleAaaa));
  ASSERT_TRUE(response.has_value());
  ASSERT_EQ(response->answers.size(), 4u);

  EXPECT_EQ(response->answers[0].type, Type::AAAA);
  EXPECT_EQ(response->answers[0].address, "2a00:1450:4010:c0f::71");
  EXPECT_EQ(response->answers[3].address, "2a00:1450:4010:c0f::64");
}

// No record of the type asked for is not an error: it is an empty answer, and RFC 3263
// moves on to the next step.
TEST(DnsMessageTest, ANameWithNothingOfTheTypeAskedForIsAnEmptyAnswer) {
  const auto response = decode_response(from_hex(kIptelNoNaptr));
  ASSERT_TRUE(response.has_value());

  EXPECT_EQ(response->rcode, 0);
  EXPECT_TRUE(response->answers.empty());
}

// The response is anybody's to write. Every way of running off the end is refused, and so
// is a pointer that points at itself, which would otherwise never finish.
TEST(DnsMessageTest, AMessageCutShortIsRefusedWherever) {
  const auto full = from_hex(kSip2SipNaptr);

  for (std::size_t length = 0; length < full.size(); ++length) {
    const std::vector<std::uint8_t> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(length));
    EXPECT_FALSE(decode_response(cut).has_value()) << "decoded a message cut to " << length << " bytes";
  }
}

TEST(DnsMessageTest, ACompressionLoopIsRefused) {
  // Header with one answer and no question; the answer's name is a pointer to itself.
  const auto looped = from_hex("000181800000000100000000c00c00010001000000000004c0000201");
  EXPECT_FALSE(decode_response(looped).has_value());
}

TEST(DnsMessageTest, AQueryIsNotAResponse) { EXPECT_FALSE(decode_response(encode_query(7, "iptel.org", Type::A)).has_value()); }

// RFC 1035 4.1.1: TC says the answer did not fit in the datagram.
TEST(DnsMessageTest, TheTruncatedBitIsReported) {
  auto message = from_hex(kIptelA);
  message[2] |= 0x02;

  const auto response = decode_response(message);
  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->truncated);
}

TEST(DnsMessageTest, NxdomainIsReported) {
  auto message = from_hex(kIptelNoNaptr);
  message[3] = static_cast<std::uint8_t>((message[3] & 0xf0) | 0x03);

  const auto response = decode_response(message);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->rcode, 3);
}
