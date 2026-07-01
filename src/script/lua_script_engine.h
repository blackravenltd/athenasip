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
  using LuaCFunction = int (*)(lua_State*);

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

  // Config
  std::string _allowed_path = "/";

  // Lua State
  lua_State* _current;

  std::string execute_lua_fn(const std::string& functionName, const std::string& arg);

  // Functions
  void _register_functions();
  void _register_function(std::string name, LuaCFunction fn, uint8_t args);
  static int lua_include(lua_State* L);

  // Set and retreive this instance
  static LuaScriptEngine* get_script_engine(lua_State* L);

  // System functions

  // Logger
  void _register_logger_object();
  static int lua_log_info(lua_State* L);
  static int lua_log_debug(lua_State* L);
  static int lua_log_warn(lua_State* L);
  static int lua_log_error(lua_State* L);
};

}  // namespace athenasip::script