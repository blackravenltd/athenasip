//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "lua_script_engine.h"

#include <functional>
#include <lua.hpp>
#include <string>

#include "../loggers/logger_scoped.h"
#include "../util.h"

using namespace athenasip::loggers;

namespace athenasip::script {

LuaScriptEngine::LuaScriptEngine(std::shared_ptr<Logger> logger) : _logger(std::make_shared<LoggerScoped>("lua", logger)), _current(nullptr) {}

LuaScriptEngine::~LuaScriptEngine() { stop(); }

void LuaScriptEngine::start() {
  if (_current) {
    stop();
  }

  _current = luaL_newstate();
  // TODO: This should be configurable
  // luaL_openlibs(_current);

  // Register our custom functions and objects
  _register_functions();
  _register_logger_object();

  // Load routing script from file
  if (luaL_loadfile(_current, Util::expand_path("~/.athenasip/routing.lua").c_str()) != LUA_OK) {
    _logger->error("(runtime) Script load error: " + std::string(lua_tostring(_current, -1)));
    lua_pop(_current, 1);
  } else {
    // Execute the loaded chunk so that all function definitions are registered.
    if (lua_pcall(_current, 0, LUA_MULTRET, 0) != LUA_OK) {
      _logger->error("(runtime) Script execution error: " + std::string(lua_tostring(_current, -1)));
      lua_pop(_current, 1);
    }
  }

  _logger->info("(runtime) Started");

  // Execute Lua function main() with one argument.
  std::string result = execute_lua_fn("main", "sip.atheasip.org");
}

void LuaScriptEngine::reload() { start(); }

void LuaScriptEngine::stop() {
  if (!_current) return;
  lua_close(_current);
  _current = nullptr;
  _logger->info("(runtime) Stopped");
}

void LuaScriptEngine::on_message(std::shared_ptr<SIPMessage> msg) {
  // TODO: implement message handling
}

void LuaScriptEngine::send_message(std::function<void(std::shared_ptr<SIPMessage> msg)> handler) {
  // TODO: implement message sending
}

// Register Lua Functions into the Lua state.
void LuaScriptEngine::_register_functions() {
  lua_pushlightuserdata(_current, this);
  lua_pushcclosure(_current, LuaScriptEngine::lua_print, 1);
  lua_setglobal(_current, "print");

  lua_pushlightuserdata(_current, this);
  lua_pushcclosure(_current, LuaScriptEngine::lua_include, 1);
  lua_setglobal(_current, "include");
}

// Lua Functions

int LuaScriptEngine::lua_print(lua_State* L) {
  LuaScriptEngine* engine = static_cast<LuaScriptEngine*>(lua_touserdata(L, lua_upvalueindex(1)));

  std::string out;
  int nargs = lua_gettop(L);  // Number of arguments

  for (int i = 1; i <= nargs; i++) {
    if (lua_isstring(L, i)) {
      out += lua_tostring(L, i);
    } else {
      engine->_logger->warn("(runtime) print() - non-string value for argument " + std::to_string(i));
    }
    if (i < nargs) {
      out += " ";  // Append a space between arguments.
    }
  }
  engine->_logger->info(out);
  return 0;  // No values are returned to Lua.
}

std::string LuaScriptEngine::execute_lua_fn(const std::string& functionName, const std::string& arg) {
  if (!_current) {
    _logger->error("(runtime) Lua state not initialized");
    return "";
  }
  // Push the function onto the stack.
  lua_getglobal(_current, functionName.c_str());
  if (!lua_isfunction(_current, -1)) {
    _logger->error("(runtime) Function '" + functionName + "' not found");
    lua_pop(_current, 1);
    return "";
  }
  // Push the argument.
  lua_pushstring(_current, arg.c_str());
  // Call the function with 1 argument and expect 1 return value.
  if (lua_pcall(_current, 1, 1, 0) != LUA_OK) {
    _logger->error("(runtime) Error executing Lua function '" + functionName + "': " + std::string(lua_tostring(_current, -1)));
    lua_pop(_current, 1);
    return "";
  }
  std::string result;
  if (lua_isstring(_current, -1)) {
    result = lua_tostring(_current, -1);
  } else {
    // _logger->warn("Lua function '" + functionName + "' did not return a string");
  }
  lua_pop(_current, 1);  // Remove the return value.
  return result;
}

// Helper: retrieve the script engine from the upvalue.
LuaScriptEngine* LuaScriptEngine::get_script_engine(lua_State* L) {
    // Upvalue index 1 should be a lightuserdata holding the logger pointer.
    return static_cast<LuaScriptEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

}  // namespace athenasip::script
