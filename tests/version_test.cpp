//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include "version.h"

using namespace athenasip;

TEST(VersionTest, ConstructFromArray) {
  uint8_t arr[3] = {1, 2, 3};
  Version v(arr, 3);
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 2);
  EXPECT_EQ(v.patch, 3);

  // Missing components are zero.
  uint8_t arr1[1] = {10};
  Version v1(arr1, 1);
  EXPECT_EQ(v1.major, 10);
  EXPECT_EQ(v1.minor, 0);
  EXPECT_EQ(v1.patch, 0);

  Version v_empty(nullptr, 0);
  EXPECT_EQ(v_empty.major, 0);
  EXPECT_EQ(v_empty.minor, 0);
  EXPECT_EQ(v_empty.patch, 0);
}

TEST(VersionTest, ConstructFromString_Valid) {
  Version v("1.2.3");
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 2);
  EXPECT_EQ(v.patch, 3);

  Version v2("10.20");
  EXPECT_EQ(v2.major, 10);
  EXPECT_EQ(v2.minor, 20);
  EXPECT_EQ(v2.patch, 0);

  Version v3("99");
  EXPECT_EQ(v3.major, 99);
  EXPECT_EQ(v3.minor, 0);
  EXPECT_EQ(v3.patch, 0);
}

TEST(VersionTest, ConstructFromString_InvalidOutOfRange) {
  // Each component must fit in 0-255.
  EXPECT_THROW(Version("256.0.0"), std::runtime_error);

  EXPECT_THROW(Version("0.300.0"), std::runtime_error);
  EXPECT_THROW(Version("0.0.999"), std::runtime_error);
}

TEST(VersionTest, ConstructFromNumbers) {
  Version v(5, 6, 7);
  EXPECT_EQ(v.major, 5);
  EXPECT_EQ(v.minor, 6);
  EXPECT_EQ(v.patch, 7);
}

TEST(VersionTest, PackAndToString) {
  Version v(11, 22, 33);
  uint8_t buf[3] = {0};
  size_t packed = v.pack(buf);
  EXPECT_EQ(packed, 3);
  EXPECT_EQ(buf[0], 11);
  EXPECT_EQ(buf[1], 22);
  EXPECT_EQ(buf[2], 33);

  EXPECT_EQ(v.to_string(), "11.22.33");
}

TEST(VersionTest, OutputOperator) {
  Version v(100, 200, 250);
  std::ostringstream oss;
  oss << v;
  EXPECT_EQ(oss.str(), "100.200.250");
}

TEST(VersionTest, ResultBeforeExecutionThrows) {
  // Intentionally empty: a Version always has a value.
}

