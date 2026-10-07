//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using athenasip::cli::Options;
using athenasip::cli::parse;

namespace {

Options parse_of(std::vector<const char*> arguments) {
  arguments.insert(arguments.begin(), "athenasip");
  return parse(static_cast<int>(arguments.size()), const_cast<char**>(arguments.data()));
}

}  // namespace

// --config, -c and --config=PATH name the configuration file.
TEST(CliTest, TheConfigurationPathCanBeGiven) {
  EXPECT_EQ(parse_of({"--config", "/etc/athenasip/config.yaml"}).config, "/etc/athenasip/config.yaml");
  EXPECT_EQ(parse_of({"-c", "/tmp/other.yaml"}).config, "/tmp/other.yaml");
  EXPECT_EQ(parse_of({"--config=/tmp/joined.yaml"}).config, "/tmp/joined.yaml");
}

// With no arguments the path is left empty for the configuration search.
TEST(CliTest, NothingGivenLeavesThePathToTheSearch) {
  const auto options = parse_of({});

  EXPECT_TRUE(options.config.empty());
  EXPECT_TRUE(options.ok);
  EXPECT_FALSE(options.version);
  EXPECT_FALSE(options.help);
}

// --version/-v and --help/-h are recognised.
TEST(CliTest, TheVersionCanBeAskedFor) {
  EXPECT_TRUE(parse_of({"--version"}).version);
  EXPECT_TRUE(parse_of({"-v"}).version);
  EXPECT_TRUE(parse_of({"--help"}).help);
  EXPECT_TRUE(parse_of({"-h"}).help);
}

// An unknown option, or one missing its value, is an error rather than ignored.
TEST(CliTest, WhatIsNotUnderstoodIsRefused) {
  const auto unknown = parse_of({"--colour=green"});
  EXPECT_FALSE(unknown.ok);
  EXPECT_FALSE(unknown.error.empty());

  const auto empty = parse_of({"--config"});
  EXPECT_FALSE(empty.ok);
  EXPECT_NE(empty.error.find("--config"), std::string::npos);
}

// --- Creating an administrator without the API ---

TEST(CliTest, AUserToAddCanBeNamedEitherWay) {
  EXPECT_EQ(parse_of({"--add-user", "tom"}).add_user, "tom");
  EXPECT_EQ(parse_of({"--add-user=tom"}).add_user, "tom");
}

TEST(CliTest, RolesAccumulateAndADisplayNameIsOptional) {
  const auto options = parse_of({"--add-user", "tom", "--role", "manage-realms", "--role=manage-admin-users", "--display-name", "Tom Cully"});

  ASSERT_TRUE(options.ok);
  EXPECT_EQ(options.display_name, "Tom Cully");
  ASSERT_EQ(options.roles.size(), 2u);
  EXPECT_EQ(options.roles[0], "manage-realms");
  EXPECT_EQ(options.roles[1], "manage-admin-users");
}

// There is deliberately no --password: arguments are visible to every process on the host.
TEST(CliTest, ThereIsNoWayToPassAPasswordOnTheCommandLine) {
  EXPECT_FALSE(parse_of({"--add-user", "tom", "--password", "hunter2"}).ok);
  EXPECT_EQ(parse_of({"--add-user", "tom", "--password", "hunter2"}).error, "unknown option: --password");
}

TEST(CliTest, AnOptionMissingItsValueIsRefusedRatherThanIgnored) {
  EXPECT_FALSE(parse_of({"--add-user"}).ok);
  EXPECT_FALSE(parse_of({"--role"}).ok);
  EXPECT_FALSE(parse_of({"--add-user="}).ok);

  EXPECT_NE(parse_of({"--add-user"}).error.find("a username"), std::string::npos);
}

TEST(CliTest, TheUsageSaysHowThePasswordIsRead) {
  const auto usage = athenasip::cli::usage();

  EXPECT_NE(usage.find("--add-user"), std::string::npos);
  EXPECT_NE(usage.find("manage-admin-users"), std::string::npos);
  EXPECT_NE(usage.find("standard input"), std::string::npos);
}

TEST(CliTest, TheEffectiveConfigurationCanBeAskedFor) {
  EXPECT_TRUE(parse_of({"--print-config"}).print_config);
  EXPECT_FALSE(parse_of({"--print-config"}).version);

  // It combines with --config.
  const auto options = parse_of({"--config", "/etc/athenasip/config.yaml", "--print-config"});
  ASSERT_TRUE(options.ok);
  EXPECT_TRUE(options.print_config);
  EXPECT_EQ(options.config, "/etc/athenasip/config.yaml");
}

// --print-schema prints JSON by default; =markdown prints the reference. Anything else is a usage error.
TEST(CliTest, TheSettingsCanBeAskedForAsASchemaOrAReference) {
  EXPECT_EQ(parse_of({"--print-schema"}).print_schema, "json");
  EXPECT_EQ(parse_of({"--print-schema=json"}).print_schema, "json");
  EXPECT_EQ(parse_of({"--print-schema=markdown"}).print_schema, "markdown");
  EXPECT_FALSE(parse_of({"--print-schema=xml"}).ok);
  EXPECT_NE(athenasip::cli::usage().find("--print-schema"), std::string::npos);
}

// --ca-init, --ca-dir, --ca-node, --san and --replace drive the cluster CA.
TEST(CliTest, TheClusterCaCanBeMadeAndUsed) {
  const auto init = parse_of({"--ca-init", "--ca-dir", "/srv/ca"});
  ASSERT_TRUE(init.ok);
  EXPECT_TRUE(init.ca_init);
  EXPECT_EQ(init.ca_dir, "/srv/ca");

  const auto node = parse_of({"--ca-node", "node-a", "--san", "10.35.1.20", "--san=node-a.example.com", "--replace"});
  ASSERT_TRUE(node.ok);
  EXPECT_EQ(node.ca_node, "node-a");
  EXPECT_EQ(node.sans, (std::vector<std::string>{"10.35.1.20", "node-a.example.com"}));
  EXPECT_TRUE(node.replace);

  EXPECT_FALSE(parse_of({"--ca-node"}).ok);
}
