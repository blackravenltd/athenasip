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
#include <utility>
#include <vector>

namespace athenasip::plugins {

// One key of the configuration file, as the reference, the editor schema and the check for
// misspelt keys describe it. The server's own keys and each driver's are described alike.
struct Setting {
  enum class Type { Section, Boolean, Integer, String, List, Choice };

  // Dotted, from the top of the file ("sip.timers.t1_rtt_ms"). A driver's start at its own
  // section, named after the driver ("rtpengine", "rtpengine.timeout_ms"); the registry puts
  // them under the section for its kind.
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

// Shorthand for writing settings down: define::integer("rtpengine.timeout_ms", "500", "...", 1).
namespace define {

inline Setting section(std::string key, std::string description, bool open = false) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Section;
  setting.description = std::move(description);
  setting.open = open;
  return setting;
}

inline Setting boolean(std::string key, std::string fallback, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Boolean;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  return setting;
}

inline Setting integer(std::string key, std::string fallback, std::string description, std::optional<std::int64_t> minimum = std::nullopt,
                       std::optional<std::int64_t> maximum = std::nullopt, bool or_zero = false) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Integer;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  setting.minimum = minimum;
  setting.maximum = maximum;
  setting.or_zero = or_zero;
  return setting;
}

inline Setting port(std::string key, std::string fallback, std::string description) {
  return integer(std::move(key), std::move(fallback), std::move(description), 0, 65535);
}

inline Setting text(std::string key, std::string fallback, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::String;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  return setting;
}

inline Setting list(std::string key, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::List;
  setting.fallback = "[]";
  setting.description = std::move(description);
  return setting;
}

inline Setting choice(std::string key, std::string fallback, std::vector<std::string> choices, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Choice;
  setting.fallback = std::move(fallback);
  setting.choices = std::move(choices);
  setting.description = std::move(description);
  return setting;
}

inline Setting required(Setting setting) {
  setting.required = true;
  return setting;
}

}  // namespace define

}  // namespace athenasip::plugins
