//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <lua.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../policy/policy.h"

namespace athenasip::script {

// Lua 5.4 for the policy hooks. One state per load: a reload builds a new one and the old is closed when the last
// call running on it finishes, so no request changes script half way.
//
// Each hook call runs in its own coroutine. A store lookup issues the asynchronous datastore call and yields; the
// answer resumes the coroutine on the caller's executor. To the script it is a function that returned; to the node
// it is one more operation that took an executor and a handler, and the strand is never held.
//
// Everything here runs on the Core strand, which is the only thread that touches a state.
class LuaEngine : public std::enable_shared_from_this<LuaEngine> {
 public:
  struct Limits {
    // Lua instructions one hook call may run, not counting the time it waits on the store.
    std::uint64_t instructions = 1000000;

    // How long one hook call may take, waiting included.
    std::chrono::milliseconds timeout{5000};

    // Bytes one state may hold.
    std::size_t memory = 64 * 1024 * 1024;
  };

  // path: directories searched for the entry and for require, in order. The standard scripts, built into the
  // node, are searched last.
  LuaEngine(std::shared_ptr<loggers::Logger> logger, std::vector<std::string> path, std::string entry, Limits limits);
  ~LuaEngine();

  LuaEngine(const LuaEngine&) = delete;
  LuaEngine& operator=(const LuaEngine&) = delete;

  // Builds a state, runs the prelude, the entry script and its init(). Empty on success, else the error with its
  // file and line. A failed reload leaves the running state in place.
  std::string load();

  // What the store lookups and the node's own facts come from. Set before the first call.
  void attach(std::shared_ptr<policy::Host> host) { _host = std::move(host); }
  const std::shared_ptr<policy::Host>& host() const { return _host; }

  // The configuration scripts read, which outlives the engine: the host's once attached, this before then.
  void config_set(const Config* config) { _config = config; }
  const Config* config() const { return _host ? &_host->config() : _config; }

  // SHA-256 of every script the running state loaded, names and text, in the order loaded. Two nodes with the
  // same scripts have the same fingerprint.
  std::string fingerprint() const;

  // Whether the running scripts define a global function of this name.
  bool defines(const std::string& hook) const;

  // Pushes a hook's arguments onto the coroutine and returns how many. live is false once the call is over, and the
  // objects pushed refuse use after that.
  using Push = std::function<int(lua_State* thread, const std::shared_ptr<bool>& live)>;

  // Reads the hook's answer while it is still on the coroutine's stack: `results` values at the top. An empty error
  // means the hook returned; otherwise it raised, ran out of a budget, or timed out, and nothing is on the stack.
  using Finish = std::function<void(lua_State* thread, int results, const std::string& error)>;

  // Calls a global hook. finish is called once, on `on`.
  void call(plugins::Executor on, const std::string& hook, Push push, Finish finish);

  // A store lookup from inside a hook: called by the library with the coroutine that asked. `start` issues the
  // lookup and answers through the function it is given, with whether it succeeded and a pusher for its value (or
  // its error message). The coroutine yields until then.
  using Answer = std::function<void(bool ok, std::function<int(lua_State*)> push)>;
  static int await(lua_State* thread, std::function<void(Answer)> start);

  // The engine and the hook call a Lua state belongs to.
  static LuaEngine* of(lua_State* L);

  std::shared_ptr<loggers::Logger> script_logger() const { return _script_logger; }

  // The calls still running, for tests.
  std::size_t calls_running() const { return _calls.size(); }

 private:
  struct Generation;
  struct Call;

  void _step(const std::shared_ptr<Call>& call, int arguments);
  void _end(std::shared_ptr<Call> call, int results, const std::string& error);
  std::string _error_of(lua_State* main, lua_State* thread) const;
  int _load_chunk(lua_State* L, const std::string& relative);

  static void _count(lua_State* L, lua_Debug* debug);
  static Call* _call_of(lua_State* thread);
  static int _require(lua_State* L);
  static int _resumed(lua_State* L, int status, lua_KContext base);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<loggers::Logger> _script_logger;
  std::vector<std::string> _path;
  std::string _entry;
  Limits _limits;
  std::shared_ptr<policy::Host> _host;
  const Config* _config = nullptr;

  std::shared_ptr<Generation> _current;

  // The state being built by load(), whose instructions no hook call owns.
  Generation* _loading = nullptr;
  std::unordered_map<Call*, std::shared_ptr<Call>> _calls;
};

}  // namespace athenasip::script
