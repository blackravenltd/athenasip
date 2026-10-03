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
// needs - before it is started, or when it will not start and the log is not saying why.
// Nothing is changed and no listener is opened, so it is safe beside a node that is serving.
struct CheckLine {
  bool ok = false;
  std::string what;
  std::string detail;
};

// How a caller waits for an async connect, which is main's to give: the checks run where
// main's own start-up would, on the same executor.
using ConnectAndWait = std::function<plugins::Status(const std::function<void(plugins::Executor, plugins::StatusHandler)>&)>;

// Every check, in the order a node starts in. The configuration itself has been read by the
// time this is called: a file that will not load is reported before there is anything to
// check with.
std::vector<CheckLine> check(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Config> config, const ConnectAndWait& connect_and_wait);

// A URL with its password taken out, for printing.
std::string redacted(const std::string& url);

// The lines as text, one each, and whether all of them passed.
std::string report(const std::vector<CheckLine>& lines);
bool passed(const std::vector<CheckLine>& lines);

}  // namespace athenasip::cli
