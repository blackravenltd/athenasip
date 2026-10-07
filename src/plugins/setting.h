//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace athenasip::plugins {

// One key of the configuration file, as the reference, the editor schema and the check for
// misspelt keys describe it. The server's own keys and each driver's are described alike.
struct Setting {
  enum class Type { Section, Boolean, Integer, String, List, Choice };

  // Dotted, from the top of the file ("sip.timers.t1_rtt_ms"), or for a driver's own keys
  // from the top of its section ("timeout_ms").
  std::string key;
  Type type = Type::String;
  std::string description;

  // As YAML text ("500", "true", "[]"). Empty when there is none, or when it depends on
  // something else, which the description then says.
  std::string fallback;

  std::optional<std::int64_t> minimum;
  std::optional<std::int64_t> maximum;

  // Zero is accepted as well as minimum to maximum: zero usually turns the thing off.
  bool or_zero = false;

  // For Choice, the values accepted.
  std::vector<std::string> choices;

  // For List: an entry may also be a map holding its value under this key, and a single
  // value may stand for a list of one.
  std::string entry_key;
  bool or_single = false;

  // Must be given when its section is.
  bool required = false;

  // For Section: it may also hold sections that are not listed here - the drivers' own,
  // named after each driver.
  bool open = false;
};

using Settings = std::vector<Setting>;

}  // namespace athenasip::plugins
