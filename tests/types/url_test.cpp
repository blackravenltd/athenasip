//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/url.h"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>

using namespace athenasip::types;

static bool isValidURL(const std::string& urlStr) {
  URL url(urlStr);
  return url.is_valid();
}

TEST(URLTest, ValidBasicURL) {
  URL url("http://example.com/path");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "http");
  EXPECT_EQ(url.host, "example.com");
  EXPECT_EQ(url.path, "/path");
  // 80 is the http default, so to_string omits it.
  std::string toStr = url.to_string();
  EXPECT_EQ(toStr.find(":80"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, ValidURLWithUserInfoAndPort) {
  URL url("ftp://user:pass@ftp.example.com:2121/files");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "ftp");
  EXPECT_EQ(url.username.value(), "user");
  EXPECT_EQ(url.password.value(), "pass");
  EXPECT_EQ(url.host, "ftp.example.com");
  EXPECT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 2121);
  EXPECT_EQ(url.path, "/files");

  // 2121 is not the ftp default (21), so to_string includes it.
  std::string toStr = url.to_string();
  EXPECT_NE(toStr.find(":2121"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, DefaultPortNotIncluded) {
  URL url("https://secure.example.com/securepath");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "https");
  // The default port for https is 443.
  EXPECT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 443);
  std::string toStr = url.to_string();
  EXPECT_EQ(toStr.find(":443"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, QueryAndFragment) {
  URL url("https://example.com/path?key=value#section");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.query, "key=value");
  EXPECT_EQ(url.fragment, "section");

  std::string toStr = url.to_string();
  EXPECT_NE(toStr.find("?key=value"), std::string::npos);
  EXPECT_NE(toStr.find("#section"), std::string::npos);
}

TEST(URLTest, InvalidURL) {
  URL url("not a valid url");
  EXPECT_FALSE(url.is_valid());
}

TEST(URLTest, EqualityOperator) {
  URL url1("http://example.com/path");
  URL url2("http://example.com/path");
  URL url3("https://example.com/path");
  EXPECT_TRUE(url1 == url2);
  EXPECT_FALSE(url1 == url3);
}

TEST(URLTest, ConcatenationOperators) {
  URL url("http://example.com/");
  std::string extra = "extra";
  std::string concat1 = url + extra;
  std::string concat2 = extra + url;
  // Verify that the concatenated strings contain the URL's to_string() output.
  std::string urlStr = url.to_string();
  EXPECT_NE(concat1.find(urlStr), std::string::npos);
  EXPECT_NE(concat2.find(urlStr), std::string::npos);
}

// A scheme with no host is a usable URL for a driver that takes no address, such as
// the memory:// datastore.
TEST(URLTest, HostlessUrlIsValid) {
  athenasip::types::URL url("memory://");

  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "memory");
  EXPECT_EQ(url.host, "");
}

// RFC 3986 3.2.3: port = *DIGIT. A non-numeric port is a bad URL, not part of the path.
TEST(URLTest, APortThatIsNotDigitsIsNotAUrl) {
  EXPECT_FALSE(isValidURL("redis://localhost:sixthreeseven"));
  EXPECT_FALSE(isValidURL("redis://localhost:"));
  EXPECT_FALSE(isValidURL("redis://localhost:6379x"));
}

TEST(URLTest, APortOutsideTheRangeIsNotAUrl) {
  EXPECT_FALSE(isValidURL("redis://localhost:70000"));
  EXPECT_FALSE(isValidURL("redis://localhost:999999999999"));
}

// RFC 3986 3.1: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
TEST(URLTest, ASchemeIsALetterFollowedByTheSchemeCharacters) {
  EXPECT_TRUE(isValidURL("rtpengine+ng://media.example.com:22222"));
  EXPECT_FALSE(isValidURL("1redis://localhost:6379"));
  EXPECT_FALSE(isValidURL("re dis://localhost:6379"));
  EXPECT_FALSE(isValidURL("://localhost:6379"));
}

// A scheme with no well-known port has to carry the one it was given, or to_string
// hands back a URL that names no port at all.
TEST(URLTest, ASchemeWithNoDefaultPortKeepsThePortItWasGiven) {
  URL url("rtpengine://media.example.com:22222");

  ASSERT_TRUE(url.is_valid());
  ASSERT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 22222);
  EXPECT_EQ(url.to_string(), "rtpengine://media.example.com:22222");
}

// RFC 3986 3.1: "schemes are case-insensitive", which the default port has to be too.
TEST(URLTest, TheDefaultPortIsFoundWhateverTheSchemeCase) {
  URL url("REDIS://cache.example.com");

  ASSERT_TRUE(url.is_valid());
  ASSERT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 6379);
}

// RFC 3986 3.2.1: with no ':' in userinfo there is a username and no password, not an empty one.
TEST(URLTest, AUsernameWithNoPasswordHasNone) {
  URL url("redis://alice@cache.example.com");

  ASSERT_TRUE(url.is_valid());
  EXPECT_EQ(url.username.value(), "alice");
  EXPECT_FALSE(url.password.has_value());
  EXPECT_EQ(url.to_string(), "redis://alice@cache.example.com");
}

// Parsing into an object that already holds a URL must leave nothing of the old one
// behind, whether the new one parses or not.
TEST(URLTest, ParsingAgainClearsWhatWentBefore) {
  URL url("https://user:pass@example.com:8443/path?key=value#section");
  ASSERT_TRUE(url.is_valid());

  url.parse("memory://");

  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "memory");
  EXPECT_EQ(url.host, "");
  EXPECT_EQ(url.path, "");
  EXPECT_EQ(url.query, "");
  EXPECT_EQ(url.fragment, "");
  EXPECT_FALSE(url.username.has_value());
  EXPECT_FALSE(url.password.has_value());
  EXPECT_FALSE(url.port.has_value());

  url.parse("redis://localhost:not-a-port");

  EXPECT_FALSE(url.is_valid());
  EXPECT_EQ(url.scheme, "redis");
  EXPECT_EQ(url.host, "localhost");
  EXPECT_FALSE(url.port.has_value());
}

// The URLs the shipped configuration actually holds.
TEST(URLTest, TheConfiguredDriverUrlsParse) {
  URL memory("memory://");
  EXPECT_TRUE(memory.is_valid());
  EXPECT_EQ(memory.scheme, "memory");

  URL local("local://");
  EXPECT_TRUE(local.is_valid());
  EXPECT_EQ(local.scheme, "local");

  URL redis("redis://127.0.0.1:6399");
  EXPECT_TRUE(redis.is_valid());
  EXPECT_EQ(redis.scheme, "redis");
  EXPECT_EQ(redis.host, "127.0.0.1");
  ASSERT_TRUE(redis.port.has_value());
  EXPECT_EQ(redis.port.value(), 6399);

  URL mqtt("mqtt://broker.example.com");
  EXPECT_TRUE(mqtt.is_valid());
  ASSERT_TRUE(mqtt.port.has_value());
  EXPECT_EQ(mqtt.port.value(), 1883);
}
