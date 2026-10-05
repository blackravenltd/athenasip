//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../loggers/logger.h"

namespace athenasip::plugins {

// What became of one file found in plugins.path.
struct ModuleReport {
  std::string path;
  std::string name;
  bool loaded = false;

  // Why it did not load, or what it registered.
  std::string detail;
};

// Loads every .so, .dylib and .dll in the directories given, in name order, and registers what each module
// declares with the PluginRegistry. A file that is not a module, or one built against another contract
// version, is refused and reported, never fatal. Loaded modules stay loaded for the life of the process.
std::vector<ModuleReport> load_modules(std::shared_ptr<loggers::Logger> logger, const std::vector<std::string>& directories);

}  // namespace athenasip::plugins
