//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"
#include "policy/lua_policy.h"

using namespace athenasip;

namespace {

// The policy decides who may watch whom (docs/design/presence.md, The policy hook for watchers). Carol is in the
// presentity's realm; Dave is a subscriber of another realm this node serves, proven as himself.
struct WatchFixture : ProxyFixture {
  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;
  std::shared_ptr<MockConnection> dave_connection;
  std::shared_ptr<Channel> dave;
  std::filesystem::path scripts;
  int subscriptions = 0;

  WatchFixture() {
    bind_bob();
    seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    auto other = seed_realm("other.example");
    other->id = 2;
    store->realm_update(other);
    seed_subscriber(4, "sip:dave@other.example", "dave-ha1");

    carol = make_channel("192.0.2.30", &carol_connection, "tcp");
    dave = make_channel("198.51.100.40", &dave_connection, "tcp");
    on_strand([this]() {
      carol->authenticated_as("sip:carol@example.com");
      dave->authenticated_as("sip:dave@other.example");
    });

    scripts = std::filesystem::temp_directory_path() / ("athenasip-watch-" + std::to_string(::getpid()) + "-" + std::to_string(++_made));
    std::filesystem::create_directories(scripts);
  }

  ~WatchFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(scripts, ignored);
  }

  // The node on a script of the test's own.
  void script(const std::string& main) {
    std::ofstream(scripts / "main.lua") << main;
    auto lua = std::make_shared<policy::LuaPolicy>(logger, nullptr);
    YAML::Node own;
    own["path"].push_back(scripts.string());
    ASSERT_TRUE(lua->configure(own, *config)) << lua->error();
    core->policy_register(lua);
  }

  void subscribe(const std::shared_ptr<Channel>& over, const std::string& from, const std::string& presentity, const std::string& package) {
    ++subscriptions;
    std::string raw = "SUBSCRIBE " + presentity + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.99:5060;branch=z9hG4bK-watch-" + std::to_string(subscriptions) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <" + from + ">;tag=w" + std::to_string(subscriptions) + "\r\n";
    raw += "To: <" + presentity + ">\r\n";
    raw += "Call-ID: watch-" + std::to_string(subscriptions) + "\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <" + from + ">\r\n";
    raw += "Event: " + package + "\r\n";
    raw += "Expires: 600\r\n";
    raw += "\r\n";
    receive(over, raw);
  }

  // The final answer the node gave on this connection to the latest SUBSCRIBE.
  int answered(const std::shared_ptr<MockConnection>& connection) {
    int code = 0;
    for (const auto& message : written(connection)) {
      if (message->header->type != SIPHeader::Type::Response) continue;
      if (message->header->headers_map["Call-ID"][0]->to_string() != "watch-" + std::to_string(subscriptions)) continue;
      if (message->header->response_code >= 200) code = message->header->response_code;
    }
    return code;
  }

 private:
  static inline int _made = 0;
};

constexpr const char* kHooks =
    "local std = require 'athenasip.standard'\n"
    "authorize, route, on_failure, register = std.authorize, std.route, std.on_failure, std.register\n";

}  // namespace

// Without a script the rule is the decided one: the presentity's realm, proven.
TEST(WatchPolicyTest, ByDefaultOnlyTheRealmMayWatch) {
  WatchFixture f;
  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.carol_connection), 200);

  f.subscribe(f.dave, "sip:dave@other.example", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.dave_connection), 403);
}

// A script written before presence, with no watch hook, keeps the default.
TEST(WatchPolicyTest, AScriptWithoutAWatchHookKeepsTheDefault) {
  WatchFixture f;
  f.script(kHooks);

  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.carol_connection), 200);
  f.subscribe(f.dave, "sip:dave@other.example", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.dave_connection), 403);
}

// A script may widen who watches: here, a partner realm.
TEST(WatchPolicyTest, AScriptMayLetAnotherRealmWatch) {
  WatchFixture f;
  f.script(std::string(kHooks) +
           "function watch(request, watcher, presentity, package)\n"
           "  if watcher and watcher:match('@other%.example$') then return athenasip.watch.accept() end\n"
           "  return std.watch(request, watcher, presentity, package)\n"
           "end\n");

  f.subscribe(f.dave, "sip:dave@other.example", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.dave_connection), 200);
  EXPECT_FALSE(f.requests_with(f.dave_connection, "NOTIFY").empty());
}

// Or narrow it, with an answer of its own.
TEST(WatchPolicyTest, AScriptMayRefuseWithItsOwnAnswer) {
  WatchFixture f;
  f.script(std::string(kHooks) +
           "function watch(request, watcher, presentity, package)\n"
           "  if package == 'presence' then return athenasip.watch.reject(603, 'Decline') end\n"
           "  return std.watch(request, watcher, presentity, package)\n"
           "end\n");

  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "presence");
  EXPECT_EQ(f.answered(f.carol_connection), 603);
  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.carol_connection), 200);
}

// Forward: a deployment with a presence server of its own keeps it. The SUBSCRIBE goes to the presentity's devices
// as before presence.
TEST(WatchPolicyTest, AScriptMayForwardTheSubscribe) {
  WatchFixture f;
  f.script(std::string(kHooks) +
           "function watch(request, watcher, presentity, package)\n"
           "  return athenasip.watch.forward()\n"
           "end\n");

  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "presence");
  EXPECT_FALSE(f.requests_with(f.callee_connection, "SUBSCRIBE").empty());
  EXPECT_TRUE(f.store->presence_list("sip:bob@example.com", types::PresenceRecord::Kind::Subscription).empty());
}

// A hook that does not decide is the node's failure to, not a decision.
TEST(WatchPolicyTest, AHookThatDecidesNothingIs500) {
  WatchFixture f;
  f.script(std::string(kHooks) + "function watch() return 'yes' end\n");

  f.subscribe(f.carol, "sip:carol@example.com", "sip:bob@example.com", "dialog");
  EXPECT_EQ(f.answered(f.carol_connection), 500);
}
