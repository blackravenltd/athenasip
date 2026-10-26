//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// RFC 3261 25.1: a numeric header is 1*DIGIT. Content-Length, Max-Forwards and Expires are read with this, so what
// it accepts is what frames a stream.
#include "headers/uint_header.h"

#include <gtest/gtest.h>

#include <string>

using athenasip::headers::UIntHeader;

TEST(UIntHeaderTest, DigitsAreANumberAndSurroundingBlanksAreAllowed) {
  UIntHeader header;
  EXPECT_TRUE(header.parse("0"));
  EXPECT_EQ(header.value, 0u);
  EXPECT_TRUE(header.parse(" 1300 "));
  EXPECT_EQ(header.value, 1300u);
  EXPECT_TRUE(header.parse("4294967296"));
  EXPECT_EQ(header.value, 4294967296ull) << "64 bits, not 32";
  EXPECT_TRUE(header.parse("18446744073709551615"));
  EXPECT_EQ(header.value, 18446744073709551615ull);
}

// What stoul took before: trailing garbage, a sign, and a value past 64 bits wrapping. None is a number.
TEST(UIntHeaderTest, AnythingButDigitsIsNotANumber) {
  UIntHeader header;
  for (const std::string text : {"", "   ", "12abc", "-1", "+1", "1e3", "0x10", "1 2", "18446744073709551616", "abc"}) {
    EXPECT_FALSE(header.parse(text)) << "'" << text << "'";
  }
}
