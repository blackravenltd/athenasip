//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// docs/redis-scripts.md holds every script the redis:// driver sends, byte for byte, for other datastores to be
// checked against. It falls behind the code only if this test does.
#include <gtest/gtest.h>

#include <algorithm>
#include <boost/json.hpp>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string& relative) {
  std::ifstream in(std::string(ATHENA_TEST_SOURCE_DIR) + "/" + relative);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

TEST(RedisScriptsDocTest, TheDocumentHoldsEveryScriptExactlyAsSent) {
  const auto source = read_file("src/datastores/redis_datastore.cpp");
  const auto document = read_file("docs/redis-scripts.md");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(document.empty());

  std::vector<std::string> sent;
  const std::regex script(R"re(static constexpr const char\* k\w+ = R"lua\(([\s\S]*?)\)lua";)re");
  for (std::sregex_iterator it(source.begin(), source.end(), script), end; it != end; ++it) sent.push_back((*it)[1]);

  std::vector<std::string> written;
  const std::regex block("```json\n([^\n]*)\n```");
  for (std::sregex_iterator it(document.begin(), document.end(), block), end; it != end; ++it) {
    written.push_back(std::string(boost::json::parse((*it)[1].str()).as_string()));
  }

  ASSERT_FALSE(sent.empty());
  std::sort(sent.begin(), sent.end());
  std::sort(written.begin(), written.end());
  EXPECT_EQ(written, sent) << "a script changed without docs/redis-scripts.md";
}
