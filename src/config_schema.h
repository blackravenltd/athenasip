//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

#include <string>
#include <vector>

#include "plugins/setting.h"

namespace athenasip {

// Every key Config::load_from_yaml reads, with its type, default, limits and meaning. The
// reference (docs/configuration-reference.md), the editor schema
// (docs/configuration.schema.json) and the warning for a misspelt key all come from this.
const plugins::Settings& config_settings();

// Those and every registered driver's section, each after the rest of its kind's.
plugins::Settings all_config_settings();

// The configuration as a JSON Schema (draft 2020-12), for an editor to check a file against
// as it is written: `athenasip --print-schema`.
std::string config_schema_json(const plugins::Settings& settings);

// The reference, one table per section: `athenasip --print-schema=markdown`.
std::string config_reference_markdown(const plugins::Settings& settings);

// Each key in the document that no setting describes, as a message naming it and, when one
// is close, the key that was probably meant. A driver's section inside an open one is not
// checked unless its keys are among the settings.
std::vector<std::string> unknown_config_keys(const YAML::Node& root, const plugins::Settings& settings);

}  // namespace athenasip
