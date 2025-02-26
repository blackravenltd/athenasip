//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdlib>     // for getenv
#include <filesystem>
#include "util.h"      // Header for Util

using namespace athenasip;

TEST(UtilTest, ToHexVector) {
  // Given a vector with specific bytes
  std::vector<uint8_t> vec = {0x00, 0xAB, 0xCD, 0xEF};
  // Expected output: each byte printed in two-digit hex, lower-case.
  std::string expected = "00abcdef";
  EXPECT_EQ(Util::to_hex(vec), expected);
}

TEST(UtilTest, ToHexArray) {
  // Given an array of bytes.
  uint8_t arr[4] = {0x12, 0x34, 0x56, 0x78};
  // Use the function that takes a raw array and length.
  std::string expected = "12345678";
  EXPECT_EQ(Util::to_hex(arr, 4), expected);
}

TEST(UtilTest, ToUpper) {
  std::string input = "Hello World!";
  std::string expected = "HELLO WORLD!";
  EXPECT_EQ(Util::to_upper(input), expected);

  input = "123abcXYZ";
  expected = "123ABCXYZ";
  EXPECT_EQ(Util::to_upper(input), expected);
}

TEST(UtilTest, TrimDefault) {
  // Leading and trailing whitespace should be removed.
  std::string input = "   \tHello World!\r\n  ";
  std::string expected = "Hello World!";
  EXPECT_EQ(Util::trim(input), expected);
}

TEST(UtilTest, TrimCustom) {
  // Custom trimmable characters.
  std::string input = "xxHello World!yy";
  std::string expected = "Hello World!";
  EXPECT_EQ(Util::trim(input, "xy"), expected);
}

TEST(UtilTest, MD5Hash) {
  // Known MD5 hash of "hello" is "5d41402abc4b2a76b9719d911017c592"
  std::string input = "hello";
  std::string expected = "5d41402abc4b2a76b9719d911017c592";
  EXPECT_EQ(Util::md5(input), expected);
}

TEST(UtilTest, ExpandPathTilde) {
  // If the path starts with ~, expand it to the user's home directory.
  const char* home = std::getenv("HOME");
  ASSERT_NE(home, nullptr) << "HOME environment variable must be set for this test.";
  std::string input = "~/testfolder";
  std::filesystem::path expanded = Util::expand_path(input);
  // Check that the expanded path begins with the home directory.
  std::string homeStr(home);
  EXPECT_TRUE(expanded.string().find(homeStr) == 0);
}

TEST(UtilTest, ExpandPathAbsolute) {
  // If the path is absolute, it should remain absolute.
  std::string input = "/tmp/testfile";
  std::filesystem::path expanded = Util::expand_path(input);
  EXPECT_TRUE(expanded.is_absolute());
  EXPECT_EQ(expanded.string(), std::filesystem::absolute(input).string());
}

TEST(UtilTest, IsIPv4Private) {
  // Test some private IPs.
  EXPECT_TRUE(Util::is_ipv4_private("10.0.0.1"));
  EXPECT_TRUE(Util::is_ipv4_private("172.16.5.1"));
  EXPECT_TRUE(Util::is_ipv4_private("192.168.1.1"));
  // Test a public IP.
  EXPECT_FALSE(Util::is_ipv4_private("8.8.8.8"));
  // Test an invalid IP.
  EXPECT_FALSE(Util::is_ipv4_private("300.1.1.1"));
}

TEST(UtilTest, IsIPv4) {
  // Valid IPv4 addresses.
  EXPECT_TRUE(Util::is_ipv4("127.0.0.1"));
  EXPECT_TRUE(Util::is_ipv4("255.255.255.255"));
  EXPECT_TRUE(Util::is_ipv4("0.0.0.0"));
  // Invalid IPv4 addresses.
  EXPECT_FALSE(Util::is_ipv4("256.0.0.1"));
  EXPECT_FALSE(Util::is_ipv4("192.168.1"));
  EXPECT_FALSE(Util::is_ipv4("abc.def.ghi.jkl"));
  EXPECT_FALSE(Util::is_ipv4("123.456.78.90"));
}
