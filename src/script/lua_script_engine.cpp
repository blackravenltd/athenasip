//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
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

  _register_functions();
  _register_logger_object();

  if (luaL_loadfile(_current, Util::expand_path("~/.athenasip/routing.lua").c_str()) != LUA_OK) {
    _logger->error("(runtime) Script load error: " + std::string(lua_tostring(_current, -1)));
    lua_pop(_current, 1);
  } else {
    // Run the chunk so its function definitions are registered.
    if (lua_pcall(_current, 0, LUA_MULTRET, 0) != LUA_OK) {
      _logger->error("(runtime) Script execution error: " + std::string(lua_tostring(_current, -1)));
      lua_pop(_current, 1);
    }
  }

  _logger->info("(runtime) Started");

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

void LuaScriptEngine::_register_functions() {
  _register_function("print", LuaScriptEngine::lua_log_info, 1);
  _register_function("include", LuaScriptEngine::lua_include, 1);
}

void LuaScriptEngine::_register_function(std::string name, LuaCFunction fn, uint8_t args) {
  lua_pushlightuserdata(_current, this);
  lua_pushcclosure(_current, fn, args);
  lua_setglobal(_current, name.c_str());
}

std::string LuaScriptEngine::execute_lua_fn(const std::string& functionName, const std::string& arg) {
  if (!_current) {
    _logger->error("(runtime) Lua state not initialized");
    return "";
  }
  lua_getglobal(_current, functionName.c_str());
  if (!lua_isfunction(_current, -1)) {
    _logger->error("(runtime) Function '" + functionName + "' not found");
    lua_pop(_current, 1);
    return "";
  }
  lua_pushstring(_current, arg.c_str());
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

LuaScriptEngine* LuaScriptEngine::get_script_engine(lua_State* L) {
  // Upvalue 1 is a lightuserdata holding the engine pointer.
  return static_cast<LuaScriptEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

}  // namespace athenasip::script
