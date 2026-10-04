//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "loggers/logger_stdio.h"

#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <memory>
#include <sstream>
#include <string>

#include "loggers/logger_scoped.h"

using namespace athenasip;
using namespace athenasip::loggers;

namespace {

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

}  // namespace

// Text format: a time, the level, and the line.
TEST(LoggerStdIOTest, TheTextFormatIsUnchanged) {
  std::ostringstream out;
  auto logger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG, out);

  logger->info("(proxy) No subscriber for sip:nobody@example.com - 404");

  const auto lines = lines_of(out.str());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[INFO ] (proxy) No subscriber"), std::string::npos) << lines[0];
}

// JSON format: one object a line. The scope is a field of its own, not a message prefix, and nested scopes are
// kept outermost first.
TEST(LoggerStdIOTest, TheJsonFormatIsOneObjectALine) {
  std::ostringstream out;
  auto logger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG, out);
  logger->set_format(LogFormat::Json);

  auto proxy = std::make_shared<LoggerScoped>("proxy", logger);
  auto channel = std::make_shared<LoggerScoped>("channel tls://10.35.1.164:42381", proxy);

  proxy->warn("The media engine cannot produce \"webrtc\" for sip:bob@example.com");
  channel->info("Connected");
  logger->error("no scope at all");

  const auto lines = lines_of(out.str());
  ASSERT_EQ(lines.size(), 3u);

  const auto first = boost::json::parse(lines[0]).as_object();
  EXPECT_EQ(first.at("level").as_string(), "warn");
  EXPECT_EQ(first.at("scope").as_string(), "proxy");
  EXPECT_EQ(first.at("message").as_string(), "The media engine cannot produce \"webrtc\" for sip:bob@example.com");
  EXPECT_TRUE(first.at("at").is_string());

  const auto second = boost::json::parse(lines[1]).as_object();
  EXPECT_EQ(second.at("level").as_string(), "info");
  EXPECT_EQ(second.at("scope").as_string(), "proxy/channel tls://10.35.1.164:42381");
  EXPECT_EQ(second.at("message").as_string(), "Connected");

  const auto third = boost::json::parse(lines[2]).as_object();
  EXPECT_EQ(third.at("level").as_string(), "error");
  EXPECT_FALSE(third.contains("scope"));
}

// The level filters in both formats and can be changed after construction.
TEST(LoggerStdIOTest, TheLevelFiltersWhicheverTheFormat) {
  std::ostringstream out;
  auto logger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG, out);
  logger->set_format(LogFormat::Json);
  logger->set_level(LogLevel::WARN);

  logger->debug("not this");
  logger->info("nor this");
  logger->warn("this");

  EXPECT_EQ(lines_of(out.str()).size(), 1u);
}
