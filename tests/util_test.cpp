//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include "util.h"

using namespace athenasip;

TEST(UtilTest, ToHexVector) {
  std::vector<uint8_t> vec = {0x00, 0xAB, 0xCD, 0xEF};
  std::string expected = "00abcdef";
  EXPECT_EQ(Util::to_hex(vec), expected);
}

TEST(UtilTest, ToHexArray) {
  uint8_t arr[4] = {0x12, 0x34, 0x56, 0x78};
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
  std::string input = "   \tHello World!\r\n  ";
  std::string expected = "Hello World!";
  EXPECT_EQ(Util::trim(input), expected);
}

TEST(UtilTest, TrimCustom) {
  std::string input = "xxHello World!yy";
  std::string expected = "Hello World!";
  EXPECT_EQ(Util::trim(input, "xy"), expected);
}

TEST(UtilTest, MD5Hash) {
  std::string input = "hello";
  std::string expected = "5d41402abc4b2a76b9719d911017c592";
  EXPECT_EQ(Util::md5(input), expected);
}

TEST(UtilTest, ExpandPathTilde) {
  const char* home = std::getenv("HOME");
  ASSERT_NE(home, nullptr) << "HOME environment variable must be set for this test.";
  std::string input = "~/testfolder";
  std::filesystem::path expanded = Util::expand_path(input);
  std::string homeStr(home);
  EXPECT_TRUE(expanded.string().find(homeStr) == 0);
}

TEST(UtilTest, ExpandPathAbsolute) {
  std::string input = "/tmp/testfile";
  std::filesystem::path expanded = Util::expand_path(input);
  EXPECT_TRUE(expanded.is_absolute());
  EXPECT_EQ(expanded.string(), std::filesystem::absolute(input).string());
}

TEST(UtilTest, IsIPv4Private) {
  EXPECT_TRUE(Util::is_ipv4_private("10.0.0.1"));
  EXPECT_TRUE(Util::is_ipv4_private("172.16.5.1"));
  EXPECT_TRUE(Util::is_ipv4_private("192.168.1.1"));
  EXPECT_FALSE(Util::is_ipv4_private("8.8.8.8"));
  EXPECT_FALSE(Util::is_ipv4_private("300.1.1.1"));
}

TEST(UtilTest, IsIPv4) {
  EXPECT_TRUE(Util::is_ipv4("127.0.0.1"));
  EXPECT_TRUE(Util::is_ipv4("255.255.255.255"));
  EXPECT_TRUE(Util::is_ipv4("0.0.0.0"));
  EXPECT_FALSE(Util::is_ipv4("256.0.0.1"));
  EXPECT_FALSE(Util::is_ipv4("192.168.1"));
  EXPECT_FALSE(Util::is_ipv4("abc.def.ghi.jkl"));
  EXPECT_FALSE(Util::is_ipv4("123.456.78.90"));
}

// Review finding 29: a realm is a host name (RFC 1123), or an IPv4 address, so it carries no colon and a key built
// as realm:user is read one way only, and a SIP URI built on it parses.
TEST(UtilTest, ARealmNameIsAHostName) {
  for (const auto* name : {"example.com", "sip.example.co.nz", "a", "x-1.example", "192.0.2.10", "EXAMPLE.COM"}) EXPECT_TRUE(Util::is_realm_name(name)) << name;
  for (const auto* name : {"", "a:b", "-leading.example", "trailing-.example", "two..dots", ".dot", "dot.", "with space", "at@example.com",
                           "crlf\r\n.example", "under_score.example"}) {
    EXPECT_FALSE(Util::is_realm_name(name)) << name;
  }
  EXPECT_FALSE(Util::is_realm_name(std::string(64, 'a') + ".example")) << "a label is at most 63";
  EXPECT_FALSE(Util::is_realm_name(std::string(254, 'a'))) << "a name is at most 253";
}

// RFC 3261 25.1: user = 1*( unreserved / escaped / user-unreserved ). No @, no colon, no space or control.
TEST(UtilTest, ASubscriberUserIsAnRfc3261User) {
  for (const auto* user : {"alice", "+15551234567", "1001", "a.b-c_d!~*'()", "&=+$,;?/", "caf%C3%A9"}) EXPECT_TRUE(Util::is_sip_user(user)) << user;
  for (const auto* user : {"", "a@b", "a:b", "with space", "tab\t", "crlf\r\n", "100%", "%zz", "<x>", "a\"b"}) EXPECT_FALSE(Util::is_sip_user(user)) << user;
}
