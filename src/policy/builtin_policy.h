//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "../loggers/logger.h"
#include "../types/url.h"
#include "policy.h"

namespace athenasip::policy {

// builtin://, the default: a node serving its own realms and nothing more, with no script.
//
// A caller whose From is in a realm here proves it with Digest in that realm; anybody may call into a realm
// here; nothing else passes. A request for a realm here goes to its subscriber's bindings; any other goes to its
// Request-URI. The standard Lua scripts reproduce this exactly, and tests/policy keeps the two in step.
class BuiltinPolicy final : public Policy {
 public:
  BuiltinPolicy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);

  std::string name() const override { return "builtin"; }
  std::string version() const override { return "1.0.0"; }

  void authorize(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<AuthDecision> handler) override;
  void route(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RouteDecision> handler) override;
  void register_(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RegisterDecision> handler) override;

 private:
  std::shared_ptr<loggers::Logger> _logger;
};

}  // namespace athenasip::policy
