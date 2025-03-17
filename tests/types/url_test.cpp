//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include "types/url.h"

using namespace athenasip::types;

// Helper: Returns whether a URL is valid
static bool isValidURL(const std::string& urlStr) {
  URL url(urlStr);
  return url.is_valid();
}

TEST(URLTest, ValidBasicURL) {
  // A simple URL with no user info, no port specified.
  URL url("http://example.com/path");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "http");
  EXPECT_EQ(url.host, "example.com");
  EXPECT_EQ(url.path, "/path");
  // Since "http" default is 80, to_string should NOT include ":80".
  std::string toStr = url.to_string();
  // Expect that the string does NOT contain ":80"
  EXPECT_EQ(toStr.find(":80"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, ValidURLWithUserInfoAndPort) {
  // A URL with user, password, and an explicit port different than the default.
  URL url("ftp://user:pass@ftp.example.com:2121/files");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "ftp");
  EXPECT_EQ(url.username.value(), "user");
  EXPECT_EQ(url.password.value(), "pass");
  EXPECT_EQ(url.host, "ftp.example.com");
  EXPECT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 2121);
  EXPECT_EQ(url.path, "/files");

  // In to_string, since port 2121 is not the default for ftp (default is 21), it should appear.
  std::string toStr = url.to_string();
  EXPECT_NE(toStr.find(":2121"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, DefaultPortNotIncluded) {
  // If the URL uses the default port, then to_string should not include the port.
  URL url("https://secure.example.com/securepath");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.scheme, "https");
  // The default port for https is 443.
  EXPECT_TRUE(url.port.has_value());
  EXPECT_EQ(url.port.value(), 443);
  std::string toStr = url.to_string();
  // Expect that the port is not explicitly appended.
  EXPECT_EQ(toStr.find(":443"), std::string::npos) << "to_string output: " << toStr;
}

TEST(URLTest, QueryAndFragment) {
  // Test parsing a URL with query parameters and fragment.
  URL url("https://example.com/path?key=value#section");
  EXPECT_TRUE(url.is_valid());
  EXPECT_EQ(url.query, "key=value");
  EXPECT_EQ(url.fragment, "section");

  // The output string should include the query and fragment.
  std::string toStr = url.to_string();
  EXPECT_NE(toStr.find("?key=value"), std::string::npos);
  EXPECT_NE(toStr.find("#section"), std::string::npos);
}

TEST(URLTest, InvalidURL) {
  // Provide an input that does not match the URL regex.
  URL url("not a valid url");
  // Expect invalid since regex match should fail.
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
