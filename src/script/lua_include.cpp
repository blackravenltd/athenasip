//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <functional>
#include <lua.hpp>
#include <string>

#include "../util.h"
#include "lua_script_engine.h"

using namespace athenasip::loggers;

namespace athenasip::script {

int LuaScriptEngine::lua_include(lua_State* L) {
  auto engine = LuaScriptEngine::get_script_engine(L);

  std::string org_filename = Util::expand_path(luaL_checkstring(L, 1));
  const char* filename = org_filename.c_str();

  std::string allowed_path = Util::expand_path(engine->_allowed_path).string();

  // include() may only load files under the allowed path.
  if (std::strncmp(filename, allowed_path.c_str(), allowed_path.length()) != 0) {
    return luaL_error(L, "include() - Access denied: file '%s' is not within allowed path '%s'", filename, allowed_path.c_str());
  }

  if (luaL_loadfile(L, filename) != LUA_OK) {
    return luaL_error(L, "include() - Error loading file '%s': %s", filename, lua_tostring(L, -1));
  }

  if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
    return luaL_error(L, "include() - Error executing file '%s': %s", filename, lua_tostring(L, -1));
  }

  return lua_gettop(L);
}

}  // namespace athenasip::script