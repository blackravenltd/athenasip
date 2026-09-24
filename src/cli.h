//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>

namespace athenasip::cli {

// What the command line said. Hand-rolled rather than taken from a library, which is
// the rule in this tree: four options are not worth a dependency.
struct Options {
  // Empty means nothing was given and the search decides (see config_search_paths).
  std::string config;

  bool version = false;
  bool help = false;

  // False stops the node before it starts. An argument it does not understand is not
  // ignored: a daemon that silently drops the one telling it where its configuration
  // is would read the wrong one and serve the wrong thing.
  bool ok = true;
  std::string error;
};

inline Options parse(int argc, char* argv[]) {
  Options options;

  const auto needs_value = [&options](const std::string& name) {
    options.ok = false;
    options.error = name + " needs a path";
  };

  for (int i = 1; i < argc && options.ok; ++i) {
    const std::string argument = argv[i];

    if (argument == "--version" || argument == "-v") {
      options.version = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else if (argument == "--config" || argument == "-c") {
      if (i + 1 >= argc) {
        needs_value(argument);
        break;
      }
      options.config = argv[++i];
    } else if (argument.rfind("--config=", 0) == 0) {
      options.config = argument.substr(std::string("--config=").size());
      if (options.config.empty()) needs_value("--config");
    } else {
      options.ok = false;
      options.error = "unknown option: " + argument;
    }
  }

  return options;
}

inline std::string usage() {
  return "Usage: athenasip [options]\n"
         "\n"
         "  -c, --config PATH  the configuration to read\n"
         "  -v, --version      print the version and exit\n"
         "  -h, --help         print this and exit\n"
         "\n"
         "With no --config, the first of these that exists is read:\n"
         "\n"
         "  $ATHENASIP_CONFIG\n"
         "  /etc/athenasip/config.yaml\n"
         "  ~/.athenasip/config.yaml\n";
}

}  // namespace athenasip::cli
