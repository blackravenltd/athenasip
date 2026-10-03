//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <set>
#include <string>

// docs/api/openapi.yaml is what the admin console builds against: its contract test checks
// every request it makes appears there. A document that does not parse, or that names one
// operation twice, breaks a build in another repository, so this one checks it first.
namespace {

YAML::Node document() { return YAML::LoadFile(std::string(ATHENA_TEST_SOURCE_DIR) + "/docs/api/openapi.yaml"); }

const std::set<std::string> kMethods = {"get", "put", "post", "delete", "patch"};

}  // namespace

TEST(OpenApiDocumentTest, ParsesAndEveryOperationIdIsUnique) {
  const auto doc = document();
  ASSERT_TRUE(doc["paths"].IsMap());

  std::set<std::string> seen;
  for (const auto& path : doc["paths"]) {
    for (const auto& entry : path.second) {
      const auto method = entry.first.as<std::string>();
      if (!kMethods.count(method)) continue;

      ASSERT_TRUE(entry.second["operationId"]) << path.first.as<std::string>() << " " << method << " has no operationId";
      const auto id = entry.second["operationId"].as<std::string>();
      EXPECT_TRUE(seen.insert(id).second) << id << " is used twice";
    }
  }
}

// The live state the node serves, by the operationIds the console was told.
TEST(OpenApiDocumentTest, DescribesTheLiveStateRoutes) {
  const auto doc = document();

  EXPECT_EQ(doc["paths"]["/calls"]["get"]["operationId"].as<std::string>(), "listCalls");
  EXPECT_EQ(doc["paths"]["/calls/{call}"]["get"]["operationId"].as<std::string>(), "getCall");
  EXPECT_EQ(doc["paths"]["/call-records"]["get"]["operationId"].as<std::string>(), "listCallRecords");
  EXPECT_EQ(doc["paths"]["/media"]["get"]["operationId"].as<std::string>(), "getMediaEngine");
  EXPECT_EQ(doc["paths"]["/metrics"]["get"]["operationId"].as<std::string>(), "getMetrics");

  // Outside /api/v1, so it overrides the server.
  EXPECT_TRUE(doc["paths"]["/metrics"]["servers"].IsSequence());

  // The console types the state.
  const auto states = doc["components"]["schemas"]["Call"]["properties"]["state"]["enum"];
  ASSERT_TRUE(states.IsSequence());
  EXPECT_EQ(states.size(), 6u);
}
