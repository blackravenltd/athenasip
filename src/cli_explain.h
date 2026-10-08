//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "loggers/logger.h"

namespace athenasip {
class Core;
}

namespace athenasip::cli {

// Where the request is taken to have come from: transport, address and port.
struct ExplainSource {
  std::string transport = "udp";
  std::string address = "192.0.2.1";
  std::uint16_t port = 5060;
};

// "udp:203.0.113.5:5060", "tls:203.0.113.5" or "203.0.113.5"; empty when it does not parse.
std::optional<ExplainSource> parse_explain_source(const std::string& text);

struct Explained {
  // The request parsed and the policy answered every question it was asked.
  bool ok = false;
  std::string text;
};

// `athenasip --explain FILE`: a request put to the node's policy as the proxy or the registrar would put it,
// against the live store, with each decision said in words. Sends nothing and changes nothing. Called off the
// Core strand, which it waits on.
Explained explain(const std::shared_ptr<loggers::Logger>& logger, const std::shared_ptr<Core>& core, const std::string& request, const ExplainSource& source);

}  // namespace athenasip::cli
