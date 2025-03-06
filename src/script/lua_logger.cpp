//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <functional>
#include <lua.hpp>
#include <string>

#include "../loggers/logger_scoped.h"
#include "../util.h"
#include "lua_script_engine.h"

using namespace athenasip::loggers;

namespace athenasip::script {

// Each logging function: they expect a string as the first argument.
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

// This function registers the global "log" object.
void LuaScriptEngine::_register_logger_object() {
  // Create a new table.
  lua_newtable(_current);  // table is at stack index -1

  // Push our logger pointer as a lightuserdata upvalue.
  lua_pushlightuserdata(_current, this);

  // Now, push each logging function with the logger pointer as its upvalue.
  lua_pushcclosure(_current, LuaScriptEngine::lua_log_info, 1);
  lua_setfield(_current, -2, "info");

  lua_pushlightuserdata(_current, _logger.get());
  lua_pushcclosure(_current, LuaScriptEngine::lua_log_debug, 1);
  lua_setfield(_current, -2, "debug");

  lua_pushlightuserdata(_current, _logger.get());
  lua_pushcclosure(_current, LuaScriptEngine::lua_log_warn, 1);
  lua_setfield(_current, -2, "warn");

  lua_pushlightuserdata(_current, _logger.get());
  lua_pushcclosure(_current, LuaScriptEngine::lua_log_error, 1);
  lua_setfield(_current, -2, "error");

  // Set the table as a global variable called "log".
  lua_setglobal(_current, "log");
}

}  // namespace athenasip::script