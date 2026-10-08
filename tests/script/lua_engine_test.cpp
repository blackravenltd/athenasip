//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "script/lua_engine.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../mocks/logger_mock.h"
#include "config.h"
#include "policy/policy.h"
#include "script/lua_library.h"

using namespace athenasip;
using script::LuaEngine;

namespace {

// A store answered on the test's own executor, never inline. A realm named "down" fails; one named "never" is
// never answered.
class FakeHost final : public policy::Host {
 public:
  FakeHost(boost::asio::io_context& io, std::shared_ptr<Config> config) : _io(io), _config(std::move(config)) {}

  std::map<std::string, std::shared_ptr<types::Realm>> realms;

  void realm(std::string name, plugins::Handler<std::shared_ptr<types::Realm>> handler) override {
    if (name == "never") return;
    boost::asio::post(_io, [this, name, handler]() {
      if (name == "down") return handler(plugins::Result<std::shared_ptr<types::Realm>>::failure("connection refused"));
      const auto found = realms.find(name);
      handler(plugins::Result<std::shared_ptr<types::Realm>>::success(found == realms.end() ? nullptr : found->second));
    });
  }

  void subscriber(std::shared_ptr<types::SIPIdentity>, plugins::Handler<std::shared_ptr<types::Subscriber>> handler) override {
    boost::asio::post(_io, [handler]() { handler(plugins::Result<std::shared_ptr<types::Subscriber>>::success(nullptr)); });
  }

  void locations(std::uint64_t, plugins::Handler<std::vector<types::Location>> handler) override {
    boost::asio::post(_io, [handler]() { handler(plugins::Result<std::vector<types::Location>>::success({})); });
  }

  bool names_this_node(const std::string& host, std::uint16_t port) const override { return host == "192.0.2.1" && port == 5060; }

  const Config& config() const override { return *_config; }

 private:
  boost::asio::io_context& _io;
  std::shared_ptr<Config> _config;
};

// A directory of scripts that is gone when the test is.
struct Scripts {
  std::filesystem::path root;

  Scripts() {
    root = std::filesystem::temp_directory_path() / ("athenasip-lua-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()));
    std::filesystem::create_directories(root);
  }

  ~Scripts() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void write(const std::string& name, const std::string& body) const {
    const auto file = root / name;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << body;
  }
};

struct EngineFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  boost::asio::io_context io;
  std::shared_ptr<Config> config = std::make_shared<Config>(logger);
  std::shared_ptr<FakeHost> host = std::make_shared<FakeHost>(io, config);
  Scripts scripts;
  std::shared_ptr<LuaEngine> engine;

  // Loads main.lua from the fixture's directory. Empty on success.
  std::string load(const std::string& main, LuaEngine::Limits limits = {}) {
    scripts.write("main.lua", main);
    engine = std::make_shared<LuaEngine>(logger, std::vector<std::string>{scripts.root.string()}, "main.lua", limits);
    engine->attach(host);
    return engine->load();
  }

  struct Outcome {
    bool finished = false;
    std::string error;
    std::vector<std::string> values;
  };

  // Calls a hook with no arguments and runs the executor until it finishes. Each value returned is kept as text.
  Outcome call(const std::string& hook, LuaEngine::Push push = nullptr, std::chrono::milliseconds wait = std::chrono::seconds(5)) {
    auto outcome = std::make_shared<Outcome>();
    engine->call(io.get_executor(), hook, std::move(push), [outcome](lua_State* L, int results, const std::string& error) {
      outcome->finished = true;
      outcome->error = error;
      if (L == nullptr) return;
      const int base = lua_gettop(L) - results;
      for (int i = 1; i <= results; ++i) {
        outcome->values.push_back(luaL_tolstring(L, base + i, nullptr));
        lua_pop(L, 1);
      }
    });

    io.restart();
    const auto until = std::chrono::steady_clock::now() + wait;
    while (!outcome->finished && std::chrono::steady_clock::now() < until) io.run_for(std::chrono::milliseconds(10));
    return *outcome;
  }
};

}  // namespace

TEST(LuaEngineTest, AHookReturnsItsValues) {
  EngineFixture f;
  ASSERT_EQ(f.load("function answer() return 42, 'yes' end"), "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_EQ(outcome.error, "");
  EXPECT_EQ(outcome.values, (std::vector<std::string>{"42", "yes"}));
}

// The store answers asynchronously; to the script the lookup is a function that returned.
TEST(LuaEngineTest, AStoreLookupYieldsAndResumesWithTheAnswer) {
  EngineFixture f;
  f.host->realms["example.com"] = std::make_shared<types::Realm>("example.com");
  ASSERT_EQ(f.load(R"(
    function answer()
      local found = athenasip.store.realm("example.com")
      local missing = athenasip.store.realm("example.net")
      return found.name, tostring(missing)
    end
  )"),
            "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_EQ(outcome.error, "");
  EXPECT_EQ(outcome.values, (std::vector<std::string>{"example.com", "nil"}));
  EXPECT_EQ(f.engine->calls_running(), 0u);
}

// A store that cannot answer is an error in the script, which pcall can catch like any other.
TEST(LuaEngineTest, AStoreFailureIsRaisedInTheScript) {
  EngineFixture f;
  ASSERT_EQ(f.load(R"(
    function answer()
      local ok, why = pcall(athenasip.store.realm, "down")
      return ok, why
    end
  )"),
            "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  ASSERT_EQ(outcome.values.size(), 2u);
  EXPECT_EQ(outcome.values[0], "false");
  EXPECT_NE(outcome.values[1].find("connection refused"), std::string::npos) << outcome.values[1];
}

// An error names the script and the line, which is the whole of an operator's debugging story.
TEST(LuaEngineTest, AnErrorNamesTheScriptAndTheLine) {
  EngineFixture f;
  ASSERT_EQ(f.load("function answer()\n  local x = nil\n  return x.field\nend\n"), "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_NE(outcome.error.find("main.lua:3"), std::string::npos) << outcome.error;
}

TEST(LuaEngineTest, AHookTheScriptsDoNotDefineIsAnError) {
  EngineFixture f;
  ASSERT_EQ(f.load("-- nothing"), "");

  const auto outcome = f.call("route");
  ASSERT_TRUE(outcome.finished);
  EXPECT_NE(outcome.error.find("define no route()"), std::string::npos) << outcome.error;
}

// A script cannot spin: the instructions it runs are counted, and the call is stopped past the limit.
TEST(LuaEngineTest, AScriptThatSpinsIsStopped) {
  EngineFixture f;
  LuaEngine::Limits limits;
  limits.instructions = 100000;
  ASSERT_EQ(f.load("function answer() while true do end end", limits), "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_NE(outcome.error.find("instructions"), std::string::npos) << outcome.error;
}

// A call waiting on a store that never answers is abandoned at its deadline; a late answer finds nothing.
TEST(LuaEngineTest, ACallThatWaitsTooLongIsAbandoned) {
  EngineFixture f;
  LuaEngine::Limits limits;
  limits.timeout = std::chrono::milliseconds(50);
  ASSERT_EQ(f.load("function answer() return athenasip.store.realm('never') end", limits), "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_NE(outcome.error.find("took longer than 50 ms"), std::string::npos) << outcome.error;
  EXPECT_EQ(f.engine->calls_running(), 0u);
}

// The state's memory is counted; a script that asks for too much gets an error, the node does not.
TEST(LuaEngineTest, AScriptThatAsksForTooMuchMemoryIsRefused) {
  EngineFixture f;
  LuaEngine::Limits limits;
  limits.memory = 2 * 1024 * 1024;
  ASSERT_EQ(f.load("function answer() local t = {} for i = 1, 10000000 do t[i] = i end end", limits), "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_NE(outcome.error.find("memory"), std::string::npos) << outcome.error;
}

// No io, no os beyond the clock, no loading of code from text or files, no debug, no package.
TEST(LuaEngineTest, AScriptReachesTheOutsideOnlyThroughTheLibrary) {
  EngineFixture f;
  ASSERT_EQ(f.load(R"(
    function answer()
      return type(io), type(os.execute), type(os.getenv), type(os.date), type(load), type(loadfile), type(dofile), type(debug),
             type(package)
    end
  )"),
            "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_EQ(outcome.values, (std::vector<std::string>{"nil", "nil", "nil", "function", "nil", "nil", "nil", "nil", "nil"}));
}

// require reads only from the path and the standard scripts, and only names that stay inside them.
TEST(LuaEngineTest, RequireReadsFromThePathFirstAndNowhereElse) {
  EngineFixture f;
  f.scripts.write("site/numbers.lua", "return {country = '44'}");
  ASSERT_EQ(f.load(R"(
    local numbers = require "site.numbers"
    function answer()
      local escaped = pcall(require, "..secrets")
      local standard = type(require("athenasip.standard").route)
      return numbers.country, escaped, standard
    end
  )"),
            "");

  const auto outcome = f.call("answer");
  ASSERT_TRUE(outcome.finished);
  EXPECT_EQ(outcome.values, (std::vector<std::string>{"44", "false", "function"}));
}

TEST(LuaEngineTest, AScriptThatDoesNotCompileIsNotLoaded) {
  EngineFixture f;
  const auto error = f.load("function answer( return end");
  EXPECT_NE(error.find("main.lua:1"), std::string::npos) << error;
}

TEST(LuaEngineTest, AnInitThatRaisesIsNotLoaded) {
  EngineFixture f;
  const auto error = f.load("function init() error('no trunks configured') end");
  EXPECT_NE(error.find("no trunks configured"), std::string::npos) << error;
}

// A reload that fails leaves the running scripts in place; one that succeeds takes the next call.
TEST(LuaEngineTest, AFailedReloadKeepsTheRunningScripts) {
  EngineFixture f;
  ASSERT_EQ(f.load("function answer() return 'first' end"), "");

  f.scripts.write("main.lua", "function answer( return 'broken'");
  EXPECT_NE(f.engine->load(), "");
  EXPECT_EQ(f.call("answer").values, std::vector<std::string>{"first"});

  f.scripts.write("main.lua", "function answer() return 'second' end");
  EXPECT_EQ(f.engine->load(), "");
  EXPECT_EQ(f.call("answer").values, std::vector<std::string>{"second"});
}

// A call that started on the old scripts finishes on them.
TEST(LuaEngineTest, ACallInFlightFinishesOnTheScriptsItStartedOn) {
  EngineFixture f;
  f.host->realms["example.com"] = std::make_shared<types::Realm>("example.com");
  ASSERT_EQ(f.load("function answer() athenasip.store.realm('example.com') return 'first' end"), "");

  auto outcome = std::make_shared<EngineFixture::Outcome>();
  f.engine->call(f.io.get_executor(), "answer", nullptr, [outcome](lua_State* L, int results, const std::string& error) {
    outcome->finished = true;
    outcome->error = error;
    if (results > 0) outcome->values.push_back(lua_tostring(L, -1));
  });
  ASSERT_FALSE(outcome->finished) << "the call is waiting on the store";

  f.scripts.write("main.lua", "function answer() return 'second' end");
  ASSERT_EQ(f.engine->load(), "");

  f.io.restart();
  f.io.run_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(outcome->finished);
  EXPECT_EQ(outcome->values, std::vector<std::string>{"first"});
}

// What a hook is handed belongs to that call. Kept in a global and used later, it refuses.
TEST(LuaEngineTest, ARequestKeptPastItsCallIsRefused) {
  EngineFixture f;
  ASSERT_EQ(f.load(R"(
    kept = nil
    function keep(request) kept = request return request.method end
    function later() return kept.method end
  )"),
            "");

  auto view = std::make_shared<policy::RequestView>();
  view->message = std::make_shared<SIPMessage>();
  view->message->header = std::make_shared<SIPHeader>("INVITE sip:bob@example.com SIP/2.0\r\nCall-ID: kept\r\n");

  const auto first = f.call("keep", [view](lua_State* L, const std::shared_ptr<bool>& live) { return script::push_request(L, view, live), 1; });
  EXPECT_EQ(first.values, std::vector<std::string>{"INVITE"});

  const auto second = f.call("later");
  EXPECT_NE(second.error.find("ended"), std::string::npos) << second.error;
}
