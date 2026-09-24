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
