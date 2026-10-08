//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "lua_engine.h"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "../loggers/logger_scoped.h"
#include "../util.h"
#include "lua_library.h"
#include "standard_scripts.h"

namespace athenasip::script {

namespace {

// Instructions between checks of the budget.
constexpr int kStep = 1000;

constexpr char kEngineKey[] = "athenasip.engine";
constexpr char kLoadedKey[] = "athenasip.loaded";

const char* standard_script(const std::string& name) {
  for (const auto& script : kStandardScripts) {
    if (name == script.name) return script.body;
  }
  return nullptr;
}

// A module name as require takes it: words of letters, digits and underscores, joined by dots. Nothing that could
// climb out of a directory on the path.
bool valid_module(const std::string& name) {
  if (name.empty() || name.front() == '.' || name.back() == '.') return false;

  char previous = 0;
  for (const char c : name) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!word && c != '.') return false;
    if (c == '.' && previous == '.') return false;
    previous = c;
  }
  return true;
}

}  // namespace

struct LuaEngine::Generation {
  lua_State* L = nullptr;
  std::size_t used = 0;
  std::size_t limit = 0;

  // Instructions run while loading, which no hook call owns.
  std::uint64_t loading = 0;

  // Every script read while loading, name and text, for the fingerprint.
  std::string sources;
  std::string fingerprint;

  ~Generation() {
    if (L) lua_close(L);
  }

  // Lua's allocator, counting every byte against the limit. A refused allocation is a Lua error in the script that
  // asked, not a crash.
  static void* allocate(void* self, void* block, std::size_t old_size, std::size_t new_size) {
    auto* generation = static_cast<Generation*>(self);
    const std::size_t old = block ? old_size : 0;

    if (new_size == 0) {
      generation->used -= old;
      std::free(block);
      return nullptr;
    }

    if (new_size > old && generation->used - old + new_size > generation->limit) return nullptr;

    void* moved = std::realloc(block, new_size);
    if (moved == nullptr) return nullptr;

    generation->used = generation->used - old + new_size;
    return moved;
  }
};

struct LuaEngine::Call {
  std::shared_ptr<Generation> generation;
  lua_State* thread = nullptr;
  int ref = LUA_NOREF;

  plugins::Executor on;
  std::string hook;

  std::uint64_t instructions = 0;
  std::uint64_t limit = 0;

  // A store lookup is outstanding, so a yield is expected.
  bool waiting = false;
  bool ended = false;

  // What the objects handed to the hook check before use.
  std::shared_ptr<bool> live = std::make_shared<bool>(true);

  std::shared_ptr<boost::asio::steady_timer> deadline;
  Finish finish;
};

LuaEngine::LuaEngine(std::shared_ptr<loggers::Logger> logger, std::vector<std::string> path, std::string entry, Limits limits)
    : _logger(std::make_shared<loggers::LoggerScoped>("lua", logger)),
      _script_logger(std::make_shared<loggers::LoggerScoped>("script", logger)),
      _path(std::move(path)),
      _entry(std::move(entry)),
      _limits(limits) {}

LuaEngine::~LuaEngine() {
  for (auto& [raw, call] : _calls) {
    call->ended = true;
    *call->live = false;
    if (call->deadline) call->deadline->cancel();
  }
  _calls.clear();
}

LuaEngine* LuaEngine::of(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kEngineKey);
  auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return engine;
}

LuaEngine::Call* LuaEngine::_call_of(lua_State* thread) { return *static_cast<Call**>(lua_getextraspace(thread)); }

void LuaEngine::_count(lua_State* L, lua_Debug* debug) {
  (void)debug;

  if (Call* call = _call_of(L)) {
    call->instructions += kStep;
    if (call->instructions > call->limit) luaL_error(L, "%s() ran past its limit of %d instructions", call->hook.c_str(), static_cast<int>(call->limit));
    return;
  }

  auto* engine = of(L);
  if (engine == nullptr || !engine->_loading) return;

  engine->_loading->loading += kStep;
  if (engine->_loading->loading > engine->_limits.instructions) {
    luaL_error(L, "loading ran past its limit of %d instructions", static_cast<int>(engine->_limits.instructions));
  }
}

// Reads a script from the first directory on the path that has it, else from the standard scripts built into the
// node. Pushes the compiled chunk; text only, never bytecode, which can break the interpreter.
int LuaEngine::_load_chunk(lua_State* L, const std::string& relative) {
  for (const auto& directory : _path) {
    const auto file = std::filesystem::path(directory) / relative;

    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) continue;

    std::ifstream in(file, std::ios::binary);
    std::stringstream body;
    body << in.rdbuf();
    const auto text = body.str();
    if (_loading) _loading->sources += relative + "\n" + text + "\n";

    return luaL_loadbufferx(L, text.data(), text.size(), ("@" + file.string()).c_str(), "t");
  }

  if (const char* body = standard_script(relative)) {
    if (_loading) _loading->sources += relative + "\n" + body + "\n";
    return luaL_loadbufferx(L, body, std::char_traits<char>::length(body), ("@(standard)/" + relative).c_str(), "t");
  }

  lua_pushstring(L, (relative + " is not in policy.lua.path or the standard scripts").c_str());
  return LUA_ERRFILE;
}

// require, confined to the path and the standard scripts. A module runs once and is kept.
int LuaEngine::_require(lua_State* L) {
  const std::string name = luaL_checkstring(L, 1);
  if (!valid_module(name)) return luaL_error(L, "'%s' is not a module name", name.c_str());

  lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);
  const int loaded = lua_gettop(L);

  if (lua_getfield(L, loaded, name.c_str()) != LUA_TNIL) return 1;
  lua_pop(L, 1);

  std::string relative = name;
  for (auto& c : relative) {
    if (c == '.') c = '/';
  }
  relative += ".lua";

  auto* engine = of(L);
  if (engine->_load_chunk(L, relative) != LUA_OK) return lua_error(L);

  lua_pushstring(L, name.c_str());
  lua_call(L, 1, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }

  lua_pushvalue(L, -1);
  lua_setfield(L, loaded, name.c_str());
  return 1;
}

// A message handler for the calls made while loading: the error with where it came from.
static int traceback(lua_State* L) {
  const char* message = lua_tostring(L, 1);
  luaL_traceback(L, L, message ? message : "(an error that is not a string)", 1);
  return 1;
}

std::string LuaEngine::load() {
  auto generation = std::make_shared<Generation>();
  generation->limit = _limits.memory;
  generation->L = lua_newstate(&Generation::allocate, generation.get());
  if (generation->L == nullptr) return "could not create a Lua state";

  lua_State* L = generation->L;
  *static_cast<Call**>(lua_getextraspace(L)) = nullptr;

  lua_pushlightuserdata(L, this);
  lua_setfield(L, LUA_REGISTRYINDEX, kEngineKey);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, kLoadedKey);

  // No io, no package, no debug, no coroutine: a script reaches the outside only through the library.
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
  lua_pop(L, 6);

  // Of os, only the clock: os.date and os.time.
  lua_getglobal(L, LUA_OSLIBNAME);
  lua_newtable(L);
  for (const char* keep : {"date", "time", "clock"}) {
    lua_getfield(L, -2, keep);
    lua_setfield(L, -2, keep);
  }
  lua_setglobal(L, LUA_OSLIBNAME);
  lua_pop(L, 1);

  for (const char* removed : {"dofile", "loadfile", "load"}) {
    lua_pushnil(L);
    lua_setglobal(L, removed);
  }

  lua_pushcfunction(L, &LuaEngine::_require);
  lua_setglobal(L, "require");

  open_library(L);

  _loading = generation.get();
  lua_sethook(L, &LuaEngine::_count, LUA_MASKCOUNT, kStep);

  const auto run = [this, L](const std::string& relative) -> std::string {
    lua_pushcfunction(L, traceback);
    const int handler = lua_gettop(L);

    if (_load_chunk(L, relative) != LUA_OK) {
      std::string error = lua_tostring(L, -1);
      lua_settop(L, handler - 1);
      return error;
    }

    if (lua_pcall(L, 0, 0, handler) != LUA_OK) {
      std::string error = lua_tostring(L, -1);
      lua_settop(L, handler - 1);
      return error;
    }

    lua_settop(L, handler - 1);
    return {};
  };

  std::string error = run("athenasip/prelude.lua");
  if (error.empty()) error = run(_entry);

  if (error.empty() && lua_getglobal(L, "init") == LUA_TFUNCTION) {
    lua_pushcfunction(L, traceback);
    lua_insert(L, -2);
    if (lua_pcall(L, 0, 0, -2) != LUA_OK) error = std::string("init() failed - ") + lua_tostring(L, -1);
    lua_settop(L, 0);
  } else {
    lua_settop(L, 0);
  }

  lua_sethook(L, nullptr, 0, 0);
  _loading = nullptr;

  if (!error.empty()) return error;

  generation->fingerprint = Util::sha256(generation->sources);
  generation->sources.clear();
  _current = std::move(generation);
  _logger->info("Loaded " + _entry + " (" + std::to_string(_current->used / 1024) + " KB)");
  return {};
}

std::string LuaEngine::fingerprint() const { return _current ? _current->fingerprint : std::string(); }

bool LuaEngine::defines(const std::string& hook) const {
  if (!_current) return false;

  lua_State* L = _current->L;
  const bool found = lua_getglobal(L, hook.c_str()) == LUA_TFUNCTION;
  lua_pop(L, 1);
  return found;
}

void LuaEngine::call(plugins::Executor on, const std::string& hook, Push push, Finish finish) {
  if (!_current) {
    if (finish) finish(nullptr, 0, "no scripts are loaded");
    return;
  }

  auto call = std::make_shared<Call>();
  call->generation = _current;
  call->on = on;
  call->hook = hook;
  call->limit = _limits.instructions;
  call->finish = std::move(finish);

  lua_State* L = _current->L;
  call->thread = lua_newthread(L);
  call->ref = luaL_ref(L, LUA_REGISTRYINDEX);

  *static_cast<Call**>(lua_getextraspace(call->thread)) = call.get();
  lua_sethook(call->thread, &LuaEngine::_count, LUA_MASKCOUNT, kStep);

  _calls[call.get()] = call;

  std::weak_ptr<LuaEngine> weak_self = weak_from_this();
  std::weak_ptr<Call> weak_call = call;
  call->deadline = std::make_shared<boost::asio::steady_timer>(on, _limits.timeout);
  call->deadline->async_wait([weak_self, weak_call, timeout = _limits.timeout](const boost::system::error_code& ec) {
    if (ec) return;
    auto self = weak_self.lock();
    auto call = weak_call.lock();
    if (!self || !call || call->ended) return;
    self->_end(call, 0, call->hook + "() took longer than " + std::to_string(timeout.count()) + " ms");
  });

  if (lua_getglobal(call->thread, hook.c_str()) != LUA_TFUNCTION) {
    lua_settop(call->thread, 0);
    return _end(call, 0, "the scripts define no " + hook + "()");
  }

  const int arguments = push ? push(call->thread, call->live) : 0;
  _step(call, arguments);
}

int LuaEngine::_resumed(lua_State* L, int status, lua_KContext base) {
  (void)status;

  const int ok = static_cast<int>(base) + 1;
  if (!lua_toboolean(L, ok)) {
    lua_pushvalue(L, ok + 1);
    return lua_error(L);
  }
  return lua_gettop(L) - ok;
}

int LuaEngine::await(lua_State* thread, std::function<void(Answer)> start) {
  Call* raw = _call_of(thread);
  if (raw == nullptr) return luaL_error(thread, "the store can be read only inside a hook, not while loading");
  if (!lua_isyieldable(thread)) return luaL_error(thread, "the store cannot be read here: not inside a metamethod or a module's own body");

  auto* engine = of(thread);
  const auto found = engine->_calls.find(raw);
  if (found == engine->_calls.end()) return luaL_error(thread, "this hook call has ended");

  auto call = found->second;
  call->waiting = true;

  std::weak_ptr<LuaEngine> weak_engine = engine->weak_from_this();
  std::weak_ptr<Call> weak_call = call;

  // Resumed by a post, never inline: the answer may come before the yield below has happened.
  start([weak_engine, weak_call](bool ok, std::function<int(lua_State*)> push) {
    auto call = weak_call.lock();
    if (!call || call->ended) return;

    boost::asio::post(call->on, [weak_engine, call, ok, push = std::move(push)]() {
      auto engine = weak_engine.lock();
      if (!engine || call->ended) return;

      call->waiting = false;
      lua_pushboolean(call->thread, ok ? 1 : 0);
      const int pushed = push ? push(call->thread) : 0;
      engine->_step(call, 1 + pushed);
    });
  });

  return lua_yieldk(thread, 0, static_cast<lua_KContext>(lua_gettop(thread)), &LuaEngine::_resumed);
}

void LuaEngine::_step(const std::shared_ptr<Call>& call, int arguments) {
  int results = 0;
  const int status = lua_resume(call->thread, nullptr, arguments, &results);

  if (status == LUA_YIELD) {
    if (call->waiting) return;
    lua_pop(call->thread, results);
    return _end(call, 0, call->hook + "() yielded outside a store lookup");
  }

  if (status == LUA_OK) return _end(call, results, {});

  _end(call, 0, _error_of(call->generation->L, call->thread));
}

std::string LuaEngine::_error_of(lua_State* main, lua_State* thread) const {
  const char* message = lua_tostring(thread, -1);
  luaL_traceback(main, thread, message ? message : "(an error that is not a string)", 0);
  std::string out = lua_tostring(main, -1);
  lua_pop(main, 1);
  return out;
}

void LuaEngine::_end(std::shared_ptr<Call> call, int results, const std::string& error) {
  if (call->ended) return;
  call->ended = true;

  if (call->deadline) call->deadline->cancel();

  // The hook's answer is read while it is on the stack, and before anything handed to the hook is retired.
  if (auto finish = std::move(call->finish)) finish(error.empty() ? call->thread : nullptr, error.empty() ? results : 0, error);
  *call->live = false;

  *static_cast<Call**>(lua_getextraspace(call->thread)) = nullptr;
  luaL_unref(call->generation->L, LUA_REGISTRYINDEX, call->ref);
  call->ref = LUA_NOREF;

  _calls.erase(call.get());
}

}  // namespace athenasip::script
