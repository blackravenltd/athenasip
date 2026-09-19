//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>

namespace athenasip {
class Config;
}

namespace athenasip::plugins {

// The plugin contract version. Every plugin reports the version it was built against
// and the registry refuses one that does not match, because a plugin built against a
// different contract is a crash rather than a warning. Bump it whenever anything in
// this file, or in an interface derived from Plugin, changes shape.
inline constexpr std::uint32_t API_VERSION = 1;

// The kinds AthenaSIP registers today. A kind is a plain string rather than an enum so
// that a plugin can introduce one the core was not built knowing about.
namespace kinds {

inline constexpr char datastore[] = "datastore";
inline constexpr char events[] = "events";
inline constexpr char media[] = "media";

}  // namespace kinds

// What every plugin is, whatever it plugs into. Datastore, EventSystem and MediaEngine
// derive from this, and so does anything added later; the registry knows nothing else
// about them.
//
// Identity and configuration live here. The work a kind does lives on the interface for
// that kind.
class Plugin {
 public:
  virtual ~Plugin() = default;

  // What this plugs into: one of the kinds:: constants, or a kind of the plugin's own.
  virtual std::string kind() const = 0;

  // The driver's own name, short and stable: "memory", "redis", "rtpengine". It names
  // the implementation rather than the scheme it was reached by, so all three Redis
  // schemes answer "redis".
  virtual std::string name() const = 0;

  // The driver's version, which has nothing to do with the server's.
  virtual std::string version() const = 0;

  // The contract this driver was built against. A driver compiled into the server
  // answers the version it was compiled with, which is what makes the check trivial
  // now and meaningful once plugins are loaded from shared libraries.
  virtual std::uint32_t api_version() const { return API_VERSION; }

  // Configuration, called once before connect(). own_root is the config section named
  // after the driver and is empty when there is none; system is the whole server
  // configuration, for what a driver cannot be told twice (node id, timers). The URL
  // stays the selector, so a driver needing nothing else can ignore both and a
  // one-line datastore: { url: memory:// } keeps working.
  virtual bool configure(const YAML::Node& own_root, const Config& system) {
    (void)own_root;
    (void)system;
    return true;
  }

  // Whether the driver is serving right now. Cheap and non-blocking: it reports what
  // the driver already knows rather than going and asking the far end.
  virtual bool health() const { return true; }

  // For logs and the admin API: "redis 0.0.1".
  std::string describe() const { return name() + " " + version(); }
};

}  // namespace athenasip::plugins
