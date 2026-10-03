//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <vector>

namespace athenasip::cli {

// What the command line said. Hand-rolled rather than taken from a library, which is
// the rule in this tree: a handful of options are not worth a dependency.
struct Options {
  // Empty means nothing was given and the search decides (see config_search_paths).
  std::string config;

  bool version = false;
  bool help = false;

  // Print the values this node would actually run on, and exit. Not the file: the file
  // is what `cat` is for, and most of what decides a node's behaviour is a default
  // nobody wrote down.
  bool print_config = false;

  // Create an administrator and exit, without starting a single listener. This is the
  // way back in when the API cannot be reached or every admin password has been lost,
  // so it goes to the datastore and nothing else.
  std::string add_user;
  std::string display_name;
  std::vector<std::string> roles;

  // The cluster CA (see cluster_ca.h): make one, or issue a node its certificate. Neither
  // reads the configuration. The directory defaults to ~/.athenasip/ca.
  bool ca_init = false;
  std::string ca_node;
  std::string ca_dir;
  std::vector<std::string> sans;
  bool replace = false;

  // False stops the node before it starts. An argument it does not understand is not
  // ignored: a daemon that silently drops the one telling it where its configuration
  // is would read the wrong one and serve the wrong thing.
  bool ok = true;
  std::string error;
};

inline Options parse(int argc, char* argv[]) {
  Options options;

  const auto needs_value = [&options](const std::string& name, const std::string& what) {
    options.ok = false;
    options.error = name + " needs " + what;
  };

  for (int i = 1; i < argc && options.ok; ++i) {
    const std::string argument = argv[i];

    // Both spellings for everything that takes a value: --name value and --name=value are
    // each common enough that supporting one and not the other is a papercut in somebody
    // else's script.
    const auto takes = [&](const std::string& name, const std::string& what, std::string& out) -> bool {
      if (argument == name) {
        if (i + 1 >= argc) {
          needs_value(name, what);
          return true;
        }

        out = argv[++i];
        if (out.empty()) needs_value(name, what);
        return true;
      }

      const auto prefix = name + "=";
      if (argument.rfind(prefix, 0) == 0) {
        out = argument.substr(prefix.size());
        if (out.empty()) needs_value(name, what);
        return true;
      }

      return false;
    };

    std::string role;
    std::string san;

    if (argument == "--version" || argument == "-v") {
      options.version = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else if (argument == "--print-config") {
      options.print_config = true;
    } else if (takes("--config", "a path", options.config) || takes("-c", "a path", options.config)) {
      // Handled, and any error is already recorded.
    } else if (takes("--add-user", "a username", options.add_user)) {
    } else if (takes("--display-name", "a name", options.display_name)) {
    } else if (takes("--role", "a role", role)) {
      if (!role.empty()) options.roles.push_back(role);
    } else if (argument == "--ca-init") {
      options.ca_init = true;
    } else if (takes("--ca-node", "a node id", options.ca_node)) {
    } else if (takes("--ca-dir", "a path", options.ca_dir)) {
    } else if (takes("--san", "an address or host name", san)) {
      if (!san.empty()) options.sans.push_back(san);
    } else if (argument == "--replace") {
      options.replace = true;
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
         "  -c, --config PATH     the configuration to read\n"
         "  -v, --version         print the version and exit\n"
         "  -h, --help            print this and exit\n"
         "  --print-config        print the effective configuration and exit, with the\n"
         "                        file, the search path and the defaults all resolved\n"
         "\n"
         "Administration, which starts no listeners and exits when it is done:\n"
         "\n"
         "  --add-user NAME       create an administrator in the configured datastore and\n"
         "                        exit; with memory://, which keeps nothing once this\n"
         "                        process exits, create it and carry on as the node\n"
         "  --display-name NAME   what to call them, for the console\n"
         "  --role ROLE           may be given more than once; without it the new user\n"
         "                        holds manage-admin-users, because a recovery user\n"
         "                        that cannot administer anybody is not a way back in\n"
         "\n"
         "  --ca-init             make the cluster's certificate authority\n"
         "  --ca-node ID          issue node ID its certificate, signed by that authority\n"
         "  --san NAME            an address or host name the node is reached by; may be\n"
         "                        given more than once\n"
         "  --replace             issue again over an existing node certificate\n"
         "  --ca-dir PATH         where the authority lives, ~/.athenasip/ca by default\n"
         "\n"
         "The password is read from the terminal without echoing it, or read from\n"
         "standard input when that is not a terminal. It is never taken from the\n"
         "command line, which every other process on the host can read.\n"
         "\n"
         "With no --config, the first of these that exists is read:\n"
         "\n"
         "  $ATHENASIP_CONFIG\n"
         "  /etc/athenasip/config.yaml\n"
         "  ~/.athenasip/config.yaml\n";
}

}  // namespace athenasip::cli
