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

// A node that cannot be told where its configuration is cannot be installed: a service
// reads /etc, and only a person reads a home directory.
TEST(CliTest, TheConfigurationPathCanBeGiven) {
  EXPECT_EQ(parse_of({"--config", "/etc/athenasip/config.yaml"}).config, "/etc/athenasip/config.yaml");
  EXPECT_EQ(parse_of({"-c", "/tmp/other.yaml"}).config, "/tmp/other.yaml");
  EXPECT_EQ(parse_of({"--config=/tmp/joined.yaml"}).config, "/tmp/joined.yaml");
}

// Nothing given is not an error. Where it looks, and in what order, is the whole of
// how a package and a checkout can both work without either being told.
TEST(CliTest, NothingGivenLeavesThePathToTheSearch) {
  const auto options = parse_of({});

  EXPECT_TRUE(options.config.empty());
  EXPECT_TRUE(options.ok);
  EXPECT_FALSE(options.version);
  EXPECT_FALSE(options.help);
}

// The version is what a tag, CMake and this binary all have to agree on, so asking the
// binary has to be possible.
TEST(CliTest, TheVersionCanBeAskedFor) {
  EXPECT_TRUE(parse_of({"--version"}).version);
  EXPECT_TRUE(parse_of({"-v"}).version);
  EXPECT_TRUE(parse_of({"--help"}).help);
  EXPECT_TRUE(parse_of({"-h"}).help);
}

// An option that is not understood stops the node rather than being ignored. A daemon
// that silently drops the argument telling it where its configuration is would read
// the wrong one and serve the wrong thing.
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

// A password on the command line is readable by every other process on the host, so
// there is deliberately no option that takes one.
TEST(CliTest, ThereIsNoWayToPassAPasswordOnTheCommandLine) {
  EXPECT_FALSE(parse_of({"--add-user", "tom", "--password", "hunter2"}).ok);
  EXPECT_EQ(parse_of({"--add-user", "tom", "--password", "hunter2"}).error, "unknown option: --password");
}

TEST(CliTest, AnOptionMissingItsValueIsRefusedRatherThanIgnored) {
  EXPECT_FALSE(parse_of({"--add-user"}).ok);
  EXPECT_FALSE(parse_of({"--role"}).ok);
  EXPECT_FALSE(parse_of({"--add-user="}).ok);

  // A daemon that silently dropped the argument telling it what to do would do something
  // else instead, which is worse than refusing.
  EXPECT_NE(parse_of({"--add-user"}).error.find("a username"), std::string::npos);
}

TEST(CliTest, TheUsageSaysHowThePasswordIsRead) {
  const auto usage = athenasip::cli::usage();

  EXPECT_NE(usage.find("--add-user"), std::string::npos);
  EXPECT_NE(usage.find("manage-admin-users"), std::string::npos);
  EXPECT_NE(usage.find("standard input"), std::string::npos);
}
