//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media/bencode.h"

#include <gtest/gtest.h>

#include <string>

using athenasip::media::Bencode;

// Written from the bencode grammar, not from the encoder: the far end is rtpengine.

TEST(BencodeTest, IntegersAreSpelledBetweenIAndE) {
  EXPECT_EQ(Bencode(static_cast<std::int64_t>(0)).encode(), "i0e");
  EXPECT_EQ(Bencode(static_cast<std::int64_t>(42)).encode(), "i42e");
  EXPECT_EQ(Bencode(static_cast<std::int64_t>(-7)).encode(), "i-7e");
}

TEST(BencodeTest, StringsCarryTheirOwnLength) {
  EXPECT_EQ(Bencode(std::string("offer")).encode(), "5:offer");
  EXPECT_EQ(Bencode(std::string()).encode(), "0:");

  // Length-prefixed, so colons, newlines and nulls in an SDP pass through untouched.
  EXPECT_EQ(Bencode(std::string("a:b\r\nc")).encode(), "6:a:b\r\nc");
}

TEST(BencodeTest, ListsAndDictionariesNest) {
  auto list = Bencode::list({Bencode(std::string("origin")), Bencode(static_cast<std::int64_t>(1))});
  EXPECT_EQ(list.encode(), "l6:origini1ee");

  auto dictionary = Bencode::dictionary({{"command", Bencode(std::string("offer"))}});
  dictionary.set("replace", list);

  EXPECT_EQ(dictionary.encode(), "d7:command5:offer7:replacel6:origini1eee");
}

// Canonical bencode puts dictionary keys in byte order, so a request encodes the same way every time.
TEST(BencodeTest, DictionaryKeysAreEmittedInByteOrder) {
  auto dictionary = Bencode::dictionary({{"sdp", Bencode(std::string("v=0"))}, {"command", Bencode(std::string("offer"))}});
  dictionary.set("call-id", Bencode(std::string("x")));

  EXPECT_EQ(dictionary.encode(), "d7:call-id1:x7:command5:offer3:sdp3:v=0e");
}

TEST(BencodeTest, SettingAKeyTwiceReplacesIt) {
  auto dictionary = Bencode::dictionary({});
  dictionary.set("result", Bencode(std::string("error")));
  dictionary.set("result", Bencode(std::string("ok")));

  EXPECT_EQ(dictionary.encode(), "d6:result2:oke");
}

TEST(BencodeTest, DecodeReadsBackWhatEncodeWrote) {
  auto original = Bencode::dictionary({{"command", Bencode(std::string("answer"))},
                                       {"call-id", Bencode(std::string("abc@example.com"))},
                                       {"flags", Bencode::list({Bencode(std::string("trust address"))})},
                                       {"count", Bencode(static_cast<std::int64_t>(-3))}});

  auto decoded = Bencode::decode(original.encode());
  ASSERT_TRUE(decoded.has_value());

  EXPECT_EQ(decoded->string_at("command"), "answer");
  EXPECT_EQ(decoded->string_at("call-id"), "abc@example.com");
  EXPECT_EQ(decoded->integer_at("count"), -3);

  const auto* flags = decoded->find("flags");
  ASSERT_NE(flags, nullptr);
  ASSERT_TRUE(flags->is_list());
  ASSERT_EQ(flags->values().size(), 1u);
  EXPECT_EQ(flags->values()[0].string(), "trust address");

  EXPECT_EQ(decoded->encode(), original.encode());
}

TEST(BencodeTest, AnAbsentOrWronglyTypedFieldReadsAsMissing) {
  auto dictionary = Bencode::dictionary({{"result", Bencode(std::string("ok"))}});

  EXPECT_EQ(dictionary.find("sdp"), nullptr);
  EXPECT_EQ(dictionary.string_at("sdp"), "");

  // A field that is present but not a number yields the fallback, not 0.
  EXPECT_EQ(dictionary.integer_at("result", -1), -1);
}

// The input is a datagram off a socket: anything malformed is refused, not guessed at.
TEST(BencodeTest, MalformedInputIsRefused) {
  EXPECT_FALSE(Bencode::decode("").has_value());
  EXPECT_FALSE(Bencode::decode("x").has_value());

  // A length that runs past the end of the buffer.
  EXPECT_FALSE(Bencode::decode("10:short").has_value());

  EXPECT_FALSE(Bencode::decode("i42").has_value());
  EXPECT_FALSE(Bencode::decode("ie").has_value());
  EXPECT_FALSE(Bencode::decode("l5:offer").has_value());
  EXPECT_FALSE(Bencode::decode("d3:key").has_value());

  // A dictionary key has to be a string.
  EXPECT_FALSE(Bencode::decode("di1e3:abce").has_value());

  // A number has one spelling: no leading zeros.
  EXPECT_FALSE(Bencode::decode("i007e").has_value());
  EXPECT_FALSE(Bencode::decode("i-0e").has_value());
  EXPECT_FALSE(Bencode::decode("03:abc").has_value());

  // Trailing data that is not padding.
  EXPECT_FALSE(Bencode::decode("i1ei2e").has_value());
}

TEST(BencodeTest, AnIntegerTooLargeToHoldIsRefusedRatherThanWrapped) {
  EXPECT_FALSE(Bencode::decode("i99999999999999999999e").has_value());
  EXPECT_FALSE(Bencode::decode("99999999999999999999:x").has_value());
}

// A datagram may arrive padded with trailing nulls or whitespace.
TEST(BencodeTest, TrailingPaddingIsAccepted) {
  auto decoded = Bencode::decode(std::string("d6:result2:oke\0\0", 16));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->string_at("result"), "ok");
}

// Nesting depth is bounded, so a datagram of nothing but "l" cannot exhaust the stack.
TEST(BencodeTest, NestingDeeperThanTheLimitIsRefused) {
  const std::string bomb(Bencode::kMaxDepth + 8, 'l');
  EXPECT_FALSE(Bencode::decode(bomb).has_value());

  std::string deep(Bencode::kMaxDepth + 8, 'l');
  deep += std::string(Bencode::kMaxDepth + 8, 'e');
  EXPECT_FALSE(Bencode::decode(deep).has_value());

  // Within the limit still parses.
  std::string fine(4, 'l');
  fine += std::string(4, 'e');
  EXPECT_TRUE(Bencode::decode(fine).has_value());
}

// The shape of an rtpengine query reply, reduced to the fields this driver reads.
TEST(BencodeTest, DecodesTheShapeOfAnRtpengineQuery) {
  auto stream =
      Bencode::dictionary({{"local port", Bencode(static_cast<std::int64_t>(30000))}, {"last packet", Bencode(static_cast<std::int64_t>(1700000042))}});

  auto media = Bencode::dictionary({{"index", Bencode(static_cast<std::int64_t>(1))}, {"streams", Bencode::list({stream})}});

  auto leg = Bencode::dictionary({{"medias", Bencode::list({media})}});

  auto reply = Bencode::dictionary(
      {{"result", Bencode(std::string("ok"))}, {"created", Bencode(static_cast<std::int64_t>(1700000000))}, {"tags", Bencode::dictionary({{"alice", leg}})}});

  auto decoded = Bencode::decode(reply.encode());
  ASSERT_TRUE(decoded.has_value());

  const auto* tags = decoded->find("tags");
  ASSERT_NE(tags, nullptr);
  ASSERT_TRUE(tags->is_dictionary());

  const auto* found = tags->find("alice");
  ASSERT_NE(found, nullptr);

  const auto* medias = found->find("medias");
  ASSERT_NE(medias, nullptr);
  ASSERT_EQ(medias->values().size(), 1u);

  const auto* streams = medias->values()[0].find("streams");
  ASSERT_NE(streams, nullptr);
  ASSERT_EQ(streams->values().size(), 1u);

  EXPECT_EQ(streams->values()[0].integer_at("last packet"), 1700000042);
}
