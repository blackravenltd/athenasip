//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// scripts/athenasip/trunks.lua: trunks driven by their records alone, and the examples in docs/scripting/examples.
#include <gtest/gtest.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../helpers/core_fixture_helper.h"
#include "../helpers/policy_host_helper.h"
#include "policy/lua_policy.h"
#include "script/lua_engine.h"

using namespace athenasip;

namespace {

struct TrunksScriptFixture : CoreFixture {
  std::shared_ptr<TestHost> host;
  std::shared_ptr<policy::LuaPolicy> lua;
  std::shared_ptr<Channel> carrier;
  std::shared_ptr<Channel> phone;
  std::filesystem::path scripts;

  TrunksScriptFixture() {
    host = std::make_shared<TestHost>(core);

    auto realm = seed_realm("example.com");
    auto alice = seed_subscriber(1, "sip:alice@example.com", "alice-ha1");
    phone = make_channel("192.0.2.10");
    register_binding(alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060"), phone, 3600);
    carrier = make_channel("203.0.113.5");

    add_trunk("acme", "sip:sip.acme.example", R"({"prefixes": ["+44"], "country": "44", "caller_id": "+442012345678",
                                                  "numbers": {"+442071234567": "sip:alice@example.com"}})",
              {"203.0.113.0/24"});
    add_trunk("globex", "sip:sip.globex.example", R"({"prefixes": ["+44", "+1"], "priority": 10, "dial_format": "digits"})", {});
    add_trunk("initech", "sip:sip.initech.example", R"({"prefixes": ["+4420"], "priority": 50})", {});

    scripts = std::filesystem::temp_directory_path() / ("athenasip-trunks-" + std::to_string(::getpid()));
    std::filesystem::create_directories(scripts);
    std::ofstream(scripts / "main.lua") << "authorize, route, on_failure, register = require('athenasip.trunks').hooks()\n";

    lua = std::make_shared<policy::LuaPolicy>(logger, nullptr);
    YAML::Node own;
    own["path"].push_back(scripts.string());
    EXPECT_TRUE(lua->configure(own, *config)) << lua->error();
    lua->attach(host);
  }

  ~TrunksScriptFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(scripts, ignored);
  }

  void add_trunk(const std::string& name, const std::string& uri, const std::string& attributes, std::vector<std::string> inbound) {
    auto trunk = std::make_shared<types::Trunk>();
    trunk->name = name;
    trunk->uri = uri;
    trunk->contact_user = "4420";
    trunk->inbound_addresses = std::move(inbound);
    trunk->attributes = boost::json::parse(attributes).as_object();
    EXPECT_TRUE(store->trunk_create(trunk));
  }

  std::shared_ptr<policy::RequestView> invite(const std::string& uri, const std::string& from, const std::shared_ptr<Channel>& over,
                                              const std::string& to = "") {
    std::string raw = "INVITE " + uri + " SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.99;branch=z9hG4bK-t\r\n";
    raw += "From: <" + from + ">;tag=f\r\nTo: <" + (to.empty() ? uri : to) + ">\r\nCall-ID: trunks\r\nCSeq: 1 INVITE\r\n";
    auto view = std::make_shared<policy::RequestView>();
    view->message = std::make_shared<SIPMessage>();
    view->message->header = std::make_shared<SIPHeader>(raw);
    view->message->channel = over;
    return view;
  }

  plugins::Result<policy::AuthDecision> authorize(const std::shared_ptr<policy::RequestView>& request) {
    return ask_policy<policy::AuthDecision>(core, [&](plugins::Executor on, plugins::Handler<policy::AuthDecision> h) { lua->authorize(on, request, h); });
  }

  plugins::Result<policy::RouteDecision> route(const std::shared_ptr<policy::RequestView>& request) {
    return ask_policy<policy::RouteDecision>(core, [&](plugins::Executor on, plugins::Handler<policy::RouteDecision> h) { lua->route(on, request, h); });
  }
};

}  // namespace

// A carrier is recognised by where its request came from, and the number it brings names a subscriber.
TEST(TrunksScriptTest, ACallInFromACarrierReachesTheSubscriberItsNumberNames) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:+442071234567@192.0.2.1", "sip:+447700900123@sip.acme.example", f.carrier);

  const auto auth = f.authorize(request);
  ASSERT_TRUE(auth.ok) << auth.error;
  ASSERT_EQ(auth.value.kind, policy::AuthDecision::Kind::Trusted);
  EXPECT_EQ(auth.value.trunk, "acme");

  request->trunk = auth.value.trunk;
  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.targets.size(), 1u);
  EXPECT_EQ(route.value.targets[0].kind, policy::Target::Kind::Subscriber);
  EXPECT_EQ(route.value.targets[0].subscriber->id, 1u);
}

// A carrier the node registers to sends to the Contact registered, with the number dialled in the To.
TEST(TrunksScriptTest, ANumberInTheToIsReadWhenTheRequestUriIsTheRegisteredContact) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:4420@192.0.2.1", "sip:+447700900123@sip.acme.example", f.carrier, "sip:02071234567@sip.acme.example");
  request->trunk = "acme";

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.targets.size(), 1u) << "020 7123 4567 is +442071234567 in country 44";
  EXPECT_EQ(route.value.targets[0].subscriber->id, 1u);
}

// Unset, the Contact registered is the trunk's username (RFC 3261 10.2.1), and the script must know it as that.
TEST(TrunksScriptTest, TheRegisteredContactIsTheUsernameWhenNoContactUserIsSet) {
  TrunksScriptFixture f;
  auto trunk = std::make_shared<types::Trunk>();
  trunk->name = "hooli";
  trunk->uri = "sip:sip.hooli.example";
  trunk->username = "7788";
  trunk->register_enabled = true;
  trunk->attributes = boost::json::parse(R"({"numbers": {"+442071234567": "sip:alice@example.com"}})").as_object();
  ASSERT_TRUE(f.store->trunk_create(trunk));

  auto request = f.invite("sip:7788@192.0.2.1", "sip:+447700900123@sip.hooli.example", f.carrier, "sip:+442071234567@sip.hooli.example");
  request->trunk = "hooli";

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.targets.size(), 1u) << "the number is in the To; the Request-URI is the Contact registered as the username";
  EXPECT_EQ(route.value.targets[0].subscriber->id, 1u);
}

TEST(TrunksScriptTest, ANumberNoListNamesIsRefused) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:+442079999999@192.0.2.1", "sip:+447700900123@sip.acme.example", f.carrier);
  request->trunk = "acme";

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  EXPECT_EQ(route.value.kind, policy::RouteDecision::Kind::Reply);
  EXPECT_EQ(route.value.code, 404);
}

// Out: every trunk that carries the number, longest prefix first, then lowest priority, each with the number as
// it wants it, and the caller ID of the first asserted to the carrier.
TEST(TrunksScriptTest, ACallOutTriesTheBestTrunkFirst) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:020%207123%200000@example.com", "sip:alice@example.com", f.phone);
  request->message->header->request_uri->user = "020 7123 0000";

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.kind, policy::RouteDecision::Kind::Forward);
  // Only acme has a country to read a national number by: +442071230000. globex and initech cannot read 020.
  ASSERT_EQ(route.value.targets.size(), 1u);
  EXPECT_EQ(route.value.targets[0].trunk, "acme");
  EXPECT_EQ(route.value.targets[0].uri->user, "+442071230000");

  const auto& edits = request->edits;
  ASSERT_EQ(edits.size(), 2u);
  EXPECT_EQ(edits[0].op, policy::HeaderEdit::Op::From);
  EXPECT_EQ(*edits[0].user, "+442012345678");
  EXPECT_EQ(edits[1].name, "P-Asserted-Identity");
}

TEST(TrunksScriptTest, AnInternationalNumberGoesByTheLongestPrefixThenPriority) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:+442071230000@example.com", "sip:alice@example.com", f.phone);

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.targets.size(), 3u);
  EXPECT_EQ(route.value.targets[0].trunk, "initech") << "+4420 is longer than +44";
  EXPECT_EQ(route.value.targets[1].trunk, "globex") << "priority 10 before acme's 100";
  EXPECT_EQ(route.value.targets[1].uri->user, "442071230000") << "globex dials digits";
  EXPECT_EQ(route.value.targets[2].trunk, "acme");
}

// A stranger calling a number at one of this node's realms does not get a free call out.
TEST(TrunksScriptTest, OnlyACallerInARealmHereCallsOut) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:+442071230000@example.com", "sip:mallory@elsewhere.example", f.phone);

  const auto route = f.route(request);
  ASSERT_TRUE(route.ok) << route.error;
  EXPECT_EQ(route.value.kind, policy::RouteDecision::Kind::Reply) << "the number is no subscriber here, and the caller may not call out";
  EXPECT_EQ(route.value.code, 404);
}

// A busy or unknown number is the answer from whichever trunk gives it; a trunk failing is not.
TEST(TrunksScriptTest, ABusyNumberEndsTheSearchAndAFailingTrunkDoesNot) {
  TrunksScriptFixture f;
  auto request = f.invite("sip:+442071230000@example.com", "sip:alice@example.com", f.phone);

  const auto after = [&](int code) {
    auto response = std::make_shared<SIPMessage>();
    response->header = std::make_shared<SIPHeader>("SIP/2.0 " + std::to_string(code) + " X\r\n");
    policy::ForkState state;
    state.trunk = "initech";
    return ask_policy<policy::FailureDecision>(
        f.core, [&](plugins::Executor on, plugins::Handler<policy::FailureDecision> h) { f.lua->on_failure(on, request, response, state, h); });
  };

  EXPECT_EQ(after(486).value.kind, policy::FailureDecision::Kind::Stop);
  EXPECT_EQ(after(404).value.kind, policy::FailureDecision::Kind::Stop);
  EXPECT_EQ(after(503).value.kind, policy::FailureDecision::Kind::Next);
  EXPECT_EQ(after(408).value.kind, policy::FailureDecision::Kind::Next);
}

// Every example in the documentation loads and runs its init, so none of them is wrong on the page.
TEST(TrunksScriptTest, EveryExampleLoads) {
  auto logger = std::make_shared<MockLogger>();
  const auto examples = std::filesystem::path(ATHENA_TEST_SOURCE_DIR) / "docs" / "scripting" / "examples";

  std::size_t loaded = 0;
  for (const auto& entry : std::filesystem::directory_iterator(examples)) {
    if (entry.path().extension() != ".lua") continue;
    script::LuaEngine engine(logger, {examples.string()}, entry.path().filename().string(), {});
    EXPECT_EQ(engine.load(), "") << entry.path().filename();
    EXPECT_TRUE(engine.defines("route")) << entry.path().filename();
    ++loaded;
  }
  EXPECT_GE(loaded, 5u);
}
