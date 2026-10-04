//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <functional>
#include <lua.hpp>
#include <string>
#include <utility>

#include "../loggers/logger_scoped.h"
#include "../util.h"
#include "lua_script_engine.h"

using namespace athenasip::loggers;

namespace athenasip::script {

// Each logging function takes the message as its first argument.
int LuaScriptEngine::lua_log_info(lua_State* L) {
  auto logger = LuaScriptEngine::get_script_engine(L)->_logger;
  const char* msg = luaL_checkstring(L, 1);
  logger->info(msg);
  return 0;
}

int LuaScriptEngine::lua_log_debug(lua_State* L) {
  auto logger = LuaScriptEngine::get_script_engine(L)->_logger;
  const char* msg = luaL_checkstring(L, 1);
  logger->debug(msg);
  return 0;
}

int LuaScriptEngine::lua_log_warn(lua_State* L) {
  auto logger = LuaScriptEngine::get_script_engine(L)->_logger;
  const char* msg = luaL_checkstring(L, 1);
  logger->warn(msg);
  return 0;
}

int LuaScriptEngine::lua_log_error(lua_State* L) {
  auto logger = LuaScriptEngine::get_script_engine(L)->_logger;
  const char* msg = luaL_checkstring(L, 1);
  logger->error(msg);
  return 0;
}

// Registers the global "log" table.
void LuaScriptEngine::_register_logger_object() {
  lua_newtable(_current);  // table is at stack index -1

  // Each closure takes the engine pointer as its upvalue (get_script_engine).
  for (const auto& [name, fn] : {std::pair<const char*, lua_CFunction>{"info", LuaScriptEngine::lua_log_info},
                                 {"debug", LuaScriptEngine::lua_log_debug},
                                 {"warn", LuaScriptEngine::lua_log_warn},
                                 {"error", LuaScriptEngine::lua_log_error}}) {
    lua_pushlightuserdata(_current, this);
    lua_pushcclosure(_current, fn, 1);
    lua_setfield(_current, -2, name);
  }

  lua_setglobal(_current, "log");
}

}  // namespace athenasip::script