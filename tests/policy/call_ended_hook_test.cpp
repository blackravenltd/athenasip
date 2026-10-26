//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// The on_call_ended hook: a script is told when a call this node held ends, for the counters it keeps.
#include <gtest/gtest.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../helpers/policy_host_helper.h"
#include "../helpers/proxy_fixture_helper.h"
#include "policy/lua_policy.h"

using namespace athenasip;

namespace {

// The standard scripts, plus an on_call_ended that counts in the datastore what it was told.
struct CallEndedFixture : ProxyFixture {
  std::shared_ptr<TestHost> host;
  std::shared_ptr<policy::LuaPolicy> lua;
  std::filesystem::path scripts;

  CallEndedFixture() {
    host = std::make_shared<TestHost>(core);

    scripts = std::filesystem::temp_directory_path() / ("athenasip-ended-" + std::to_string(::getpid()));
    std::filesystem::create_directories(scripts);
    std::ofstream(scripts / "main.lua") << R"(
      local std = require "athenasip.standard"
      authorize, route, on_failure, register, watch = std.authorize, std.route, std.on_failure, std.register, std.watch

      function on_call_ended(call)
        athenasip.store.counter_add("ended")
        athenasip.store.counter_add("ended:" .. call.id)
        athenasip.store.counter_add("ended:" .. call.state)
        for _, leg in ipairs(call.participants) do
          if leg.originator then athenasip.store.counter_add("caller:" .. leg.identity) end
        end
      end
    )";

    lua = std::make_shared<policy::LuaPolicy>(logger, nullptr);
    YAML::Node own;
    own["path"].push_back(scripts.string());
    EXPECT_TRUE(lua->configure(own, *config)) << lua->error();
    lua->attach(host);
    on_strand([this]() { core->policy_register(lua); });
    bind_bob();
  }

  ~CallEndedFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(scripts, ignored);
  }

  static std::string bye_from_caller() {
    std::string raw = "BYE sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-bye\r\n";
    raw += "Route: <sip:192.0.2.1:5060;lr>\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=bob\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 2 BYE\r\n";
    raw += "Max-Forwards: 70\r\n\r\n";
    return raw;
  }
};

}  // namespace

// A call answered and ended by a BYE is reported once, with its id, its state and who began it.
TEST(CallEndedHookTest, AScriptIsToldOnceWhenACallEnds) {
  CallEndedFixture f;
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  EXPECT_EQ(f.store->counter_get("ended"), 0) << "not over yet";

  f.receive(f.caller, CallEndedFixture::bye_from_caller());
  f.settle();

  EXPECT_EQ(f.store->counter_get("ended"), 1);
  EXPECT_EQ(f.store->counter_get("ended:call-proxy"), 1);
  EXPECT_EQ(f.store->counter_get("ended:Closed"), 1);
  EXPECT_EQ(f.store->counter_get("caller:sip:alice@example.com"), 1);
}

// A script without the hook is left alone: nothing is asked of it and nothing is logged.
TEST(CallEndedHookTest, AScriptWithoutTheHookIsLeftAlone) {
  CallEndedFixture f;
  std::ofstream(f.scripts / "main.lua") << "local std = require 'athenasip.standard'\n"
                                           "authorize, route, on_failure, register, watch = std.authorize, std.route, std.on_failure, std.register, std.watch\n";
  ASSERT_TRUE(f.lua->reload().empty()) << f.lua->reload();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.receive(f.caller, CallEndedFixture::bye_from_caller());
  f.settle();

  EXPECT_EQ(f.store->counter_get("ended"), 0);
  for (const auto& line : f.logger->lines(loggers::LogLevel::ERROR)) EXPECT_EQ(line.find("on_call_ended"), std::string::npos) << line;
}
