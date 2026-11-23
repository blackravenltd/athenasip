//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// The Redis serialization protocol (RESP2, https://redis.io/docs/latest/develop/reference/protocol-spec/) as the
// redis:// driver speaks it: a command goes as an array of bulk strings; a reply is a simple string, an error, an
// integer, a bulk string, a null or an array of those. The reader is fed whatever arrived and gives back each reply as
// it completes. It is a parser of bytes from the network, so it is bounded and refuses what it does not understand.
#include "datastores/resp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace athenasip::datastores::resp;

// Every argument is a bulk string, its length in bytes first, so a value may hold anything, CRLF included.
TEST(RespTest, ACommandIsAnArrayOfBulkStrings) {
  Request request;
  request.push("SET", "key", "a\r\nb", "EX", 60);

  EXPECT_EQ(request.payload(), "*5\r\n$3\r\nSET\r\n$3\r\nkey\r\n$4\r\na\r\nb\r\n$2\r\nEX\r\n$2\r\n60\r\n");
  EXPECT_EQ(request.commands(), 1u);
}

// Several commands go in one write, pipelined, and are answered in order.
TEST(RespTest, CommandsArePipelinedInOrder) {
  Request request;
  request.push("PING");
  request.push("GET", std::string("k"));

  EXPECT_EQ(request.payload(), "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  EXPECT_EQ(request.commands(), 2u);
}

// A command whose arguments are a list, as MGET and EVAL with its keys are.
TEST(RespTest, ARangeOfArgumentsFollowsTheCommand) {
  Request request;
  request.push_range("MGET", std::vector<std::string>{"a", "bb"});

  EXPECT_EQ(request.payload(), "*3\r\n$4\r\nMGET\r\n$1\r\na\r\n$2\r\nbb\r\n");
}

// Integers of every width and sign are written in decimal; an empty string is a bulk string of length zero.
TEST(RespTest, IntegersAndEmptyStringsAreBulkStrings) {
  Request request;
  request.push("X", std::int64_t{-5}, std::uint32_t{7}, std::size_t{0}, "");

  EXPECT_EQ(request.payload(), "*5\r\n$1\r\nX\r\n$2\r\n-5\r\n$1\r\n7\r\n$1\r\n0\r\n$0\r\n\r\n");
}

namespace {

std::vector<Reply> read_all(const std::string& bytes) {
  Decoder decoder;
  decoder.feed(bytes);
  std::vector<Reply> replies;
  while (auto reply = decoder.next()) replies.push_back(std::move(*reply));
  return replies;
}

}  // namespace

TEST(RespTest, EachKindOfReplyIsRead) {
  const auto replies = read_all("+OK\r\n-ERR wrong type\r\n:-42\r\n$5\r\nhello\r\n$-1\r\n*-1\r\n$0\r\n\r\n");
  ASSERT_EQ(replies.size(), 7u);

  EXPECT_EQ(replies[0].type, Reply::Type::SimpleString);
  EXPECT_EQ(replies[0].text, "OK");

  EXPECT_EQ(replies[1].type, Reply::Type::Error);
  EXPECT_EQ(replies[1].text, "ERR wrong type");
  EXPECT_TRUE(replies[1].is_error());

  EXPECT_EQ(replies[2].type, Reply::Type::Integer);
  EXPECT_EQ(replies[2].integer, -42);

  EXPECT_EQ(replies[3].type, Reply::Type::Bulk);
  EXPECT_EQ(replies[3].text, "hello");

  EXPECT_EQ(replies[4].type, Reply::Type::Null) << "a null bulk string: GET of a key that is not there";
  EXPECT_EQ(replies[5].type, Reply::Type::Null) << "a null array";

  EXPECT_EQ(replies[6].type, Reply::Type::Bulk) << "an empty bulk string is not a null";
  EXPECT_EQ(replies[6].text, "");
}

// An array may hold any reply, nulls and other arrays included, as MGET's and EVAL's do.
TEST(RespTest, AnArrayHoldsRepliesOfAnyKind) {
  const auto replies = read_all("*4\r\n$1\r\na\r\n$-1\r\n:3\r\n*1\r\n+x\r\n");
  ASSERT_EQ(replies.size(), 1u);

  const auto& array = replies[0];
  EXPECT_EQ(array.type, Reply::Type::Array);
  ASSERT_EQ(array.elements.size(), 4u);
  EXPECT_EQ(array.elements[0].text, "a");
  EXPECT_EQ(array.elements[1].type, Reply::Type::Null);
  EXPECT_EQ(array.elements[2].integer, 3);
  ASSERT_EQ(array.elements[3].elements.size(), 1u);
  EXPECT_EQ(array.elements[3].elements[0].text, "x");
}

// TCP delivers bytes, not replies: whatever the split, the same replies come out, each only once it is whole. A bulk
// string's length counts its bytes, so CRLF inside one is data.
TEST(RespTest, RepliesAreReadWhateverTheSplit) {
  const std::string bytes = "*2\r\n$4\r\na\r\nb\r\n:7\r\n+PONG\r\n";
  Decoder decoder;
  std::vector<Reply> replies;
  for (char byte : bytes) {
    decoder.feed(std::string(1, byte));
    while (auto reply = decoder.next()) replies.push_back(std::move(*reply));
  }

  ASSERT_EQ(replies.size(), 2u);
  ASSERT_EQ(replies[0].elements.size(), 2u);
  EXPECT_EQ(replies[0].elements[0].text, "a\r\nb");
  EXPECT_EQ(replies[0].elements[1].integer, 7);
  EXPECT_EQ(replies[1].text, "PONG");
}

// Nothing is given back before it is complete.
TEST(RespTest, AnIncompleteReplyWaitsForTheRest) {
  Decoder decoder;
  decoder.feed("$5\r\nhel");
  EXPECT_FALSE(decoder.next().has_value());
  decoder.feed("lo\r\n");
  const auto reply = decoder.next();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->text, "hello");
}

namespace {

void expect_refused(const std::string& bytes, const Limits& limits = {}) {
  Decoder decoder(limits);
  decoder.feed(bytes);
  EXPECT_THROW(
      {
        while (decoder.next()) {
        }
      },
      ProtocolError)
      << bytes;
}

}  // namespace

// What RESP2 does not define is refused, so the connection can be dropped rather than read out of step.
TEST(RespTest, WhatIsNotRespIsRefused) {
  expect_refused("?what\r\n");           // no such type
  expect_refused(":12x\r\n");            // not an integer
  expect_refused("$-2\r\n");             // a length below -1
  expect_refused("$3\r\nabcd\r\n");      // a bulk string longer than its length
  expect_refused("$abc\r\nx\r\n");       // a length that is not a number
  expect_refused("+OK\rX");              // a line ended without LF
  expect_refused("%1\r\n+a\r\n+b\r\n");  // RESP3, which this client never asks for
}

// A server, or whatever is posing as one, cannot make the reader hold more than its limits.
TEST(RespTest, TheLimitsAreHeld) {
  Limits limits;
  limits.max_bulk = 8;
  limits.max_elements = 2;
  limits.max_depth = 2;
  limits.max_line = 16;

  expect_refused("$9\r\n", limits);                    // a bulk string over the limit, refused before it arrives
  expect_refused("*3\r\n", limits);                    // an array over the limit
  expect_refused("*1\r\n*1\r\n*1\r\n:1\r\n", limits);  // nested deeper than allowed
  expect_refused("+" + std::string(17, 'a'), limits);  // a line with no end in sight
}
