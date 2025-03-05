//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <lua.hpp>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "script_engine.h"

using namespace athenasip::loggers;

namespace athenasip::script {

class LuaScriptEngine : public ScriptEngine {
 public:
  LuaScriptEngine(std::shared_ptr<Logger> logger);
  ~LuaScriptEngine();

  virtual void start() override;
  virtual void reload() override;
  virtual void stop() override;

  // Events In
  virtual void on_message(std::shared_ptr<SIPMessage> msg) override;

  // Events out
  virtual void send_message(std::function<void(std::shared_ptr<SIPMessage> msg)>) override;

 protected:
  std::shared_ptr<Logger> _logger;

  lua_State* _current;
  void _register_functions();
  std::string execute_lua_fn(const std::string& functionName, const std::string& arg);

  static int _lua_print(lua_State* L);
  int _print(lua_State* L);
};

}  // namespace athenasip::script