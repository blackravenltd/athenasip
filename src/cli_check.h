//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"

namespace athenasip::cli {

// `athenasip --check`: whether this node, with this configuration, can reach everything it
// needs. Changes nothing and opens no listener, so it is safe beside a serving node.
struct CheckLine {
  bool ok = false;
  std::string what;
  std::string detail;
};

// How to wait for an async connect; main supplies it so checks run on its executor.
using ConnectAndWait = std::function<plugins::Status(const std::function<void(plugins::Executor, plugins::StatusHandler)>&)>;

// Every check, in start-up order. The configuration must already have loaded.
std::vector<CheckLine> check(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Config> config, const ConnectAndWait& connect_and_wait);

// A URL with its password masked, for printing.
std::string redacted(const std::string& url);

// The lines as text, and whether all passed.
std::string report(const std::vector<CheckLine>& lines);
bool passed(const std::vector<CheckLine>& lines);

}  // namespace athenasip::cli
