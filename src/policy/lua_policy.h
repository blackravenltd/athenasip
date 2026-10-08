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
#include "../plugins/setting.h"
#include "../script/lua_engine.h"
#include "../types/url.h"
#include "policy.h"

namespace athenasip::policy {

// lua://: the hooks are Lua functions. The entry script, main.lua unless policy.lua.entry says otherwise, defines
// the globals authorize, route, on_failure and register; with no main.lua on policy.lua.path the standard one runs,
// which behaves exactly as builtin:// does. docs/scripting.md is the reference.
class LuaPolicy final : public Policy {
 public:
  LuaPolicy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);

  static plugins::Settings settings();

  std::string name() const override { return "lua"; }
  std::string version() const override { return "1.0.0"; }

  // Loads the scripts. A script that does not compile, or whose init() raises, stops the node with the error.
  bool configure(const YAML::Node& own_root, const Config& system) override;

  void attach(std::shared_ptr<Host> host) override;

  void authorize(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<AuthDecision> handler) override;
  void route(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RouteDecision> handler) override;
  void on_failure(plugins::Executor on, std::shared_ptr<RequestView> request, std::shared_ptr<SIPMessage> response, ForkState state,
                  plugins::Handler<FailureDecision> handler) override;
  void register_(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RegisterDecision> handler) override;

  // Loads the scripts again. The running ones stay if the new ones fail. Empty on success, else the error.
  std::string reload() { return _engine ? _engine->load() : "no scripts are loaded"; }

  const std::shared_ptr<script::LuaEngine>& engine() const { return _engine; }

  // Why configure refused, with the script's file and line; empty when it did not.
  const std::string& error() const { return _error; }

 private:
  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<loggers::Logger> _base_logger;
  std::shared_ptr<script::LuaEngine> _engine;
  std::string _error;
};

}  // namespace athenasip::policy
