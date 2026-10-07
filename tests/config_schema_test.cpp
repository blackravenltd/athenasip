//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config_schema.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "config.h"
#include "mocks/logger_mock.h"

using namespace athenasip;
using plugins::Setting;

namespace {

struct Loaded {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config = std::make_shared<Config>(logger);
  bool ok = false;

  explicit Loaded(const std::string& yaml) {
    static int counter = 0;
    const auto path =
        std::filesystem::temp_directory_path() / ("athenasip-schema-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter) + ".yaml");
    {
      std::ofstream out(path);
      out << yaml;
    }
    ok = config->load_from_yaml(path.string());
    std::filesystem::remove(path);
  }

  std::string warnings() const {
    std::string all;
    for (const auto& line : logger->lines(loggers::LogLevel::WARN)) all += line + "\n";
    return all;
  }
};

YAML::Node at(const YAML::Node& root, const std::string& key) {
  YAML::Node node = YAML::Clone(root);
  std::stringstream path(key);
  for (std::string part; std::getline(path, part, '.');) {
    if (!node.IsMap() || !node[part]) return YAML::Node(YAML::NodeType::Undefined);
    node = node[part];
  }
  return node;
}

void put(YAML::Node& root, const std::string& key, const YAML::Node& value) {
  std::vector<std::string> parts;
  std::stringstream path(key);
  for (std::string part; std::getline(path, part, '.');) parts.push_back(part);

  std::vector<YAML::Node> chain{root};
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) chain.push_back(chain.back()[parts[i]]);
  chain.back()[parts.back()] = value;
}

// A value for the setting that is valid and is not its default. Lists get a prefix, which is what sip.localnet
// needs and the others take as it comes.
YAML::Node sample(const Setting& setting) {
  switch (setting.type) {
    case Setting::Type::Boolean:
      return YAML::Node(setting.fallback != "true");
    case Setting::Type::Integer: {
      if (setting.fallback.empty()) return YAML::Node(setting.minimum.value_or(0) + 7);

      const auto fallback = std::stoll(setting.fallback);
      if (setting.or_zero && fallback == 0) return YAML::Node(*setting.minimum);
      if (setting.maximum && fallback + 1 > *setting.maximum) return YAML::Node(fallback - 1);
      return YAML::Node(fallback + 1);
    }
    case Setting::Type::Choice:
      for (const auto& value : setting.choices) {
        if (value != setting.fallback) return YAML::Node(value);
      }
      return YAML::Node(setting.fallback);
    case Setting::Type::List: {
      YAML::Node values(YAML::NodeType::Sequence);
      values.push_back("10.9.8.0/24");
      return values;
    }
    default:
      return YAML::Node("sample-" + setting.key);
  }
}

bool same(const Setting& setting, const YAML::Node& expected, const YAML::Node& actual) {
  if (!actual) return false;
  switch (setting.type) {
    case Setting::Type::Boolean:
      return expected.as<bool>() == actual.as<bool>();
    case Setting::Type::Integer:
      return expected.as<std::int64_t>() == actual.as<std::int64_t>();
    case Setting::Type::List:
      return expected.as<std::vector<std::string>>() == actual.as<std::vector<std::string>>();
    default:
      return expected.as<std::string>() == actual.as<std::string>();
  }
}

void leaves(const YAML::Node& node, const std::string& prefix, std::vector<std::string>& out) {
  for (const auto& entry : node) {
    const auto key = prefix.empty() ? entry.first.as<std::string>() : prefix + "." + entry.first.as<std::string>();
    if (entry.second.IsMap()) {
      leaves(entry.second, key, out);
    } else {
      out.push_back(key);
    }
  }
}

std::string read_file(const std::string& relative) {
  std::ifstream in(std::string(ATHENA_TEST_SOURCE_DIR) + "/" + relative);
  std::stringstream all;
  all << in.rdbuf();
  return all.str();
}

}  // namespace

// Each setting is read from where the schema says, and --print-config shows it there: a document that sets every
// one to something other than its default loads, and every value comes back.
TEST(ConfigSchemaTest, EverySettingIsReadWhereTheSchemaPutsIt) {
  YAML::Node document(YAML::NodeType::Map);
  for (const auto& setting : config_settings()) {
    if (setting.type != Setting::Type::Section) put(document, setting.key, sample(setting));
  }

  std::stringstream yaml;
  yaml << document;
  Loaded loaded(yaml.str());
  ASSERT_TRUE(loaded.ok) << loaded.logger->text() << yaml.str();
  EXPECT_EQ(loaded.warnings().find("is not a setting"), std::string::npos) << loaded.warnings();

  const auto effective = YAML::Load(loaded.config->effective_yaml());
  for (const auto& setting : config_settings()) {
    if (setting.type == Setting::Type::Section) continue;
    EXPECT_TRUE(same(setting, sample(setting), at(effective, setting.key)))
        << setting.key << " set to " << sample(setting) << ", printed as " << at(effective, setting.key);
  }
}

// The defaults the schema states are the ones a node runs with. Every section is present, holding only what it
// must, so a listener's own defaults apply.
TEST(ConfigSchemaTest, TheDefaultsAreTheOnesTheNodeUses) {
  YAML::Node document(YAML::NodeType::Map);
  for (const auto& setting : config_settings()) {
    if (setting.type == Setting::Type::Section) {
      if (!at(document, setting.key)) put(document, setting.key, YAML::Node(YAML::NodeType::Map));
    } else if (setting.required) {
      put(document, setting.key, setting.fallback.empty() ? sample(setting) : YAML::Load(setting.fallback));
    }
  }

  // Each needs something it has no default for.
  put(document, "websocket.secure_port", YAML::Node(0));
  put(document, "tls.cert_pem_filename", YAML::Node("cert.pem"));
  put(document, "tls.key_pem_filename", YAML::Node("key.pem"));

  std::stringstream yaml;
  yaml << document;
  Loaded loaded(yaml.str());
  ASSERT_TRUE(loaded.ok) << loaded.logger->text() << yaml.str();

  const auto effective = YAML::Load(loaded.config->effective_yaml());
  for (const auto& setting : config_settings()) {
    if (setting.type == Setting::Type::Section || setting.fallback.empty() || setting.required) continue;
    EXPECT_TRUE(same(setting, YAML::Load(setting.fallback), at(effective, setting.key)))
        << setting.key << " defaults to " << setting.fallback << " in the schema and " << at(effective, setting.key) << " in the node";
  }
}

// Nothing the node prints as its configuration is missing from the schema.
TEST(ConfigSchemaTest, EverythingTheNodePrintsIsDescribed) {
  Loaded loaded("sip:\n  node_id: test-node\n");
  ASSERT_TRUE(loaded.ok);

  std::vector<std::string> printed;
  leaves(YAML::Load(loaded.config->effective_yaml()), "", printed);
  ASSERT_FALSE(printed.empty());

  for (const auto& key : printed) {
    const auto described = std::any_of(config_settings().begin(), config_settings().end(), [&key](const Setting& setting) { return setting.key == key; });
    EXPECT_TRUE(described) << key;
  }
}

// A misspelt key leaves its default in place, so the node says so and names the key that was probably meant.
TEST(ConfigSchemaTest, AMisspeltKeyIsNamedWithTheOneMeant) {
  Loaded loaded("sip:\n  node_id: test-node\n  sesion_expires: 600\ntcp:\n  port: 5060\n  pubilc_port: 5080\n");
  ASSERT_TRUE(loaded.ok);

  const auto warnings = loaded.warnings();
  EXPECT_NE(warnings.find("'sip.sesion_expires' is not a setting"), std::string::npos) << warnings;
  EXPECT_NE(warnings.find("did you mean 'sip.session_expires'"), std::string::npos) << warnings;
  EXPECT_NE(warnings.find("did you mean 'tcp.public_port'"), std::string::npos) << warnings;
}

// A key in the wrong section is pointed at the one it belongs in.
TEST(ConfigSchemaTest, AKeyInTheWrongSectionIsPointedAtItsOwn) {
  Loaded loaded("sip:\n  node_id: test-node\n  media_anchor: false\nqualify_interval: 30\n");
  ASSERT_TRUE(loaded.ok);

  const auto warnings = loaded.warnings();
  EXPECT_NE(warnings.find("'sip.media_anchor' is not a setting, and is ignored - did you mean 'behaviour.media_anchor'?"), std::string::npos) << warnings;
  EXPECT_NE(warnings.find("did you mean 'behaviour.qualify_interval'"), std::string::npos) << warnings;
}

// A driver's own section belongs to the driver, and a plugin's keys are not the server's to know. A stray key
// beside the url is still the server's.
TEST(ConfigSchemaTest, ADriversOwnSectionIsLeftToTheDriver) {
  Loaded loaded(
      "sip:\n  node_id: test-node\n"
      "media:\n  url: \"rtpengine://127.0.0.1:2223\"\n  acme:\n    anything: 1\n  timeout_ms: 500\n");
  ASSERT_TRUE(loaded.ok);

  const auto warnings = loaded.warnings();
  EXPECT_EQ(warnings.find("media.acme"), std::string::npos) << warnings;
  EXPECT_NE(warnings.find("'media.timeout_ms' is not a setting"), std::string::npos) << warnings;
}

TEST(ConfigSchemaTest, TheShippedExampleHasNoUnknownKeys) {
  auto logger = std::make_shared<MockLogger>();
  Config config(logger);
  ASSERT_TRUE(config.load_from_yaml(std::string(ATHENA_TEST_SOURCE_DIR) + "/config/config.example.yaml"));

  for (const auto& line : logger->lines(loggers::LogLevel::WARN)) EXPECT_EQ(line.find("is not a setting"), std::string::npos) << line;
}

// The JSON Schema is one an editor can use: it parses, names its draft, requires sip.node_id, and refuses a key
// it does not know in a section that is not open.
TEST(ConfigSchemaTest, TheJsonSchemaDescribesTheFile) {
  const auto schema = boost::json::parse(config_schema_json(config_settings())).as_object();

  EXPECT_EQ(schema.at("$schema").as_string(), "https://json-schema.org/draft/2020-12/schema");
  EXPECT_EQ(schema.at("required").as_array().at(0).as_string(), "sip");

  const auto& sip = schema.at("properties").at("sip").as_object();
  EXPECT_EQ(sip.at("required").as_array().at(0).as_string(), "node_id");
  EXPECT_FALSE(sip.at("additionalProperties").as_bool());

  const auto& t1 = sip.at("properties").at("timers").at("properties").at("t1_rtt_ms").as_object();
  EXPECT_EQ(t1.at("type").as_string(), "integer");
  EXPECT_EQ(t1.at("default").as_int64(), 500);

  EXPECT_TRUE(schema.at("properties").at("media").at("additionalProperties").is_object());
}

// The reference and the schema in docs/ are what the server generates, so neither falls behind it. To refresh
// them: athenasip --print-schema > docs/configuration.schema.json, and --print-schema=markdown for the reference.
TEST(ConfigSchemaTest, TheDocumentedReferenceIsCurrent) {
  EXPECT_EQ(read_file("docs/configuration-reference.md"), config_reference_markdown(config_settings()));
  EXPECT_EQ(read_file("docs/configuration.schema.json"), config_schema_json(config_settings()));
}
