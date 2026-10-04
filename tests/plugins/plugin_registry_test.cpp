//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "../../src/plugins/plugin_registry.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../../src/loggers/logger_stdio.h"
#include "../../src/plugins/plugin.h"

using namespace athenasip;
using namespace athenasip::plugins;

namespace {

// A plugin of a kind the core knows nothing about: the registry must serve more than the built-in kinds.
class FakePlugin : public Plugin {
 public:
  FakePlugin(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) : _logger(std::move(logger)), _url(std::move(url)) {}

  std::string kind() const override { return "fake"; }
  std::string name() const override { return "fake"; }
  std::string version() const override { return "1.2.3"; }

  std::shared_ptr<types::URL> url() const { return _url; }

 private:
  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;
};

// Same kind, a different driver, so a lookup that returns the wrong one is visible.
class OtherPlugin : public FakePlugin {
 public:
  using FakePlugin::FakePlugin;

  std::string name() const override { return "other"; }
};

// A plugin built against another contract version, as a shared library could be. It must not run.
class StalePlugin : public FakePlugin {
 public:
  using FakePlugin::FakePlugin;

  std::uint32_t api_version() const override { return API_VERSION + 1; }
};

class PluginRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _logger = std::make_shared<loggers::LoggerStdIO>(loggers::LogLevel::ERROR);
    PluginRegistry::instance().clear();
  }

  void TearDown() override { PluginRegistry::instance().clear(); }

  std::shared_ptr<loggers::Logger> _logger;
};

TEST_F(PluginRegistryTest, CreatesTheDriverRegisteredForTheUrlScheme) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");

  auto plugin = PluginRegistry::instance().create(_logger, "fake", "thing://host:1234/path");

  ASSERT_NE(plugin, nullptr);
  EXPECT_EQ(plugin->name(), "fake");
  EXPECT_EQ(plugin->kind(), "fake");
  EXPECT_EQ(plugin->describe(), "fake 1.2.3");
}

TEST_F(PluginRegistryTest, HandsTheDriverTheParsedUrl) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");

  auto plugin = PluginRegistry::instance().create_as<FakePlugin>(_logger, "fake", "thing://host:1234/path");

  ASSERT_NE(plugin, nullptr);
  ASSERT_NE(plugin->url(), nullptr);
  EXPECT_EQ(plugin->url()->scheme, "thing");
  EXPECT_EQ(plugin->url()->host, "host");
  ASSERT_TRUE(plugin->url()->port.has_value());
  EXPECT_EQ(plugin->url()->port.value(), 1234);
}

// The registry is keyed by (kind, scheme): memory:// is both a datastore and an event system.
TEST_F(PluginRegistryTest, TheSameSchemeUnderTwoKindsAreDifferentDrivers) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "kind-a", "memory");
  PluginRegistry::instance().add<OtherPlugin>(_logger, "kind-b", "memory");

  auto a = PluginRegistry::instance().create(_logger, "kind-a", "memory://");
  auto b = PluginRegistry::instance().create(_logger, "kind-b", "memory://");

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->name(), "fake");
  EXPECT_EQ(b->name(), "other");
}

TEST_F(PluginRegistryTest, SeveralSchemesCanReachOneDriver) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing+ssl");

  EXPECT_NE(PluginRegistry::instance().create(_logger, "fake", "thing://host"), nullptr);
  EXPECT_NE(PluginRegistry::instance().create(_logger, "fake", "thing+ssl://host"), nullptr);
}

TEST_F(PluginRegistryTest, RefusesAnUnknownScheme) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");

  EXPECT_EQ(PluginRegistry::instance().create(_logger, "fake", "nosuchscheme://host"), nullptr);
}

TEST_F(PluginRegistryTest, RefusesAKnownSchemeUnderTheWrongKind) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");

  EXPECT_EQ(PluginRegistry::instance().create(_logger, "other-kind", "thing://host"), nullptr);
}

// A plugin built against a different contract version is not constructed.
TEST_F(PluginRegistryTest, RefusesAPluginBuiltAgainstAnotherContractVersion) {
  PluginRegistry::instance().add<StalePlugin>(_logger, "fake", "thing");

  EXPECT_EQ(PluginRegistry::instance().create(_logger, "fake", "thing://host"), nullptr);
}

TEST_F(PluginRegistryTest, ListsWhatIsRegistered) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "kind-a", "one");
  PluginRegistry::instance().add<FakePlugin>(_logger, "kind-a", "two");
  PluginRegistry::instance().add<OtherPlugin>(_logger, "kind-b", "three");

  const auto all = PluginRegistry::instance().list();
  EXPECT_EQ(all.size(), 3u);

  const auto a_schemes = PluginRegistry::instance().schemes("kind-a");
  ASSERT_EQ(a_schemes.size(), 2u);
  EXPECT_EQ(a_schemes[0], "one");
  EXPECT_EQ(a_schemes[1], "two");

  EXPECT_TRUE(PluginRegistry::instance().schemes("nothing-here").empty());
}

// Registering the same (kind, scheme) again replaces the driver, so a plugin can override a built-in.
TEST_F(PluginRegistryTest, ReregisteringASchemeReplacesTheDriver) {
  PluginRegistry::instance().add<FakePlugin>(_logger, "fake", "thing");
  PluginRegistry::instance().add<OtherPlugin>(_logger, "fake", "thing");

  auto plugin = PluginRegistry::instance().create(_logger, "fake", "thing://host");

  ASSERT_NE(plugin, nullptr);
  EXPECT_EQ(plugin->name(), "other");
  EXPECT_EQ(PluginRegistry::instance().schemes("fake").size(), 1u);
}

}  // namespace
