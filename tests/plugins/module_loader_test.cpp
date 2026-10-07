//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "plugins/module_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "../mocks/logger_mock.h"
#include "plugins/plugin_registry.h"
#include "push/push_service.h"

using namespace athenasip;

namespace {

std::string module_dir(const std::string& name) { return std::string(ATHENA_TEST_MODULE_DIR) + "/" + name; }

}  // namespace

// A module in plugins.path is loaded and its drivers registered like the ones built in: the push service it
// declares is created through the same registry, by its scheme.
TEST(ModuleLoaderTest, AModulesDriversAreRegisteredAndCreated) {
  auto logger = std::make_shared<MockLogger>();

  const auto reports = plugins::load_modules(logger, {module_dir("sample_module")});

  ASSERT_EQ(reports.size(), 1u);
  EXPECT_TRUE(reports[0].loaded) << reports[0].detail;
  EXPECT_EQ(reports[0].name, "sample");
  EXPECT_EQ(reports[0].detail, "registered push sample://");

  auto service = push::PushService::create_driver(logger, "sample://");
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(service->describe(), "sample 1.2.3");
  EXPECT_TRUE(service->accepts(push::Notification{"sample", "token", "", push::Notification::Reason::Request}));
}

// A module's driver describes its section as a built-in one does, so the reference, the editor schema and the
// check for misspelt keys cover it.
TEST(ModuleLoaderTest, AModulesDriverDescribesItsSection) {
  auto logger = std::make_shared<MockLogger>();

  plugins::load_modules(logger, {module_dir("sample_module")});

  const auto settings = plugins::PluginRegistry::instance().settings();
  const auto described = std::any_of(settings.begin(), settings.end(), [](const plugins::Setting& setting) { return setting.key == "push.sample.region"; });
  EXPECT_TRUE(described);
}

// A module built against another contract version is refused, and never called.
TEST(ModuleLoaderTest, AModuleOfAnotherContractVersionIsRefused) {
  auto logger = std::make_shared<MockLogger>();

  const auto reports = plugins::load_modules(logger, {module_dir("mismatched_module")});

  ASSERT_EQ(reports.size(), 1u);
  EXPECT_FALSE(reports[0].loaded);
  EXPECT_EQ(reports[0].name, "mismatched");
  EXPECT_NE(reports[0].detail.find("contract version " + std::to_string(plugins::API_VERSION + 1)), std::string::npos) << reports[0].detail;
}

// A shared library that is not a module is reported, not loaded, and not fatal.
TEST(ModuleLoaderTest, ALibraryThatIsNotAModuleIsRefused) {
  auto logger = std::make_shared<MockLogger>();

  const auto reports = plugins::load_modules(logger, {module_dir("not_a_module")});

  ASSERT_EQ(reports.size(), 1u);
  EXPECT_FALSE(reports[0].loaded);
  EXPECT_NE(reports[0].detail.find("not an AthenaSIP module"), std::string::npos) << reports[0].detail;
}

// A directory that cannot be read is skipped.
TEST(ModuleLoaderTest, AMissingDirectoryIsSkipped) {
  auto logger = std::make_shared<MockLogger>();

  EXPECT_TRUE(plugins::load_modules(logger, {"/nonexistent/athenasip/plugins"}).empty());
}
