//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// builtin:// and the standard scripts are one policy written twice. These keep them so: the same requests, against
// the same store, must get the same decisions from both, failures included. A change to either that the other does
// not follow fails here.
#include <gtest/gtest.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <string>

#include "../helpers/core_fixture_helper.h"
#include "../helpers/policy_host_helper.h"
#include "policy/builtin_policy.h"
#include "policy/lua_policy.h"
#include "util.h"

using namespace athenasip;

namespace {

std::string describe(const plugins::Result<policy::AuthDecision>& result) {
  if (!result.ok) return "failed";
  const auto& d = result.value;
  std::ostringstream out;
  out << "auth " << static_cast<int>(d.kind);
  if (d.kind == policy::AuthDecision::Kind::Digest) out << " realm=" << (d.realm ? d.realm->name : "*") << " match=" << d.from_must_match;
  if (d.kind == policy::AuthDecision::Kind::Reject) out << " " << d.code << " " << d.reason;
  return out.str();
}

std::string describe(const types::MediaPolicy& media) {
  return std::string(media.anchor ? "anchor" : "no-anchor") + "/" + types::MediaPolicy::to_string(media.profiles);
}

std::string describe(const plugins::Result<policy::RouteDecision>& result) {
  if (!result.ok) return "failed";
  const auto& d = result.value;
  std::ostringstream out;
  out << "route " << static_cast<int>(d.kind);
  if (d.kind == policy::RouteDecision::Kind::Reply) {
    out << " " << d.code << " " << d.reason;
    return out.str();
  }
  for (const auto& target : d.targets) {
    if (target.kind == policy::Target::Kind::Subscriber) {
      out << " subscriber=" << target.subscriber->id << " bindings=" << (target.bindings ? std::to_string(target.bindings->size()) : "unread");
    } else {
      out << " uri=" << target.uri->to_string() << " via=" << (target.next_hop ? target.next_hop->to_string() : "-");
    }
  }
  out << " media=" << (d.media ? describe(*d.media) : "-") << " rewrite=" << (d.rewrite_contact ? (*d.rewrite_contact ? "yes" : "no") : "-");
  return out.str();
}

std::string describe(const plugins::Result<policy::RegisterDecision>& result) {
  if (!result.ok) return "failed";
  const auto& d = result.value;
  std::ostringstream out;
  out << "register " << static_cast<int>(d.kind);
  if (d.kind == policy::RegisterDecision::Kind::Accept) {
    out << " realm=" << d.realm->name << " max=" << d.max_expires << " min=" << d.min_expires << " qualify=" << d.qualify_interval;
  }
  if (d.kind == policy::RegisterDecision::Kind::Reject) out << " " << d.code << " " << d.reason;
  return out.str();
}

struct EquivalenceFixture : CoreFixture {
  std::shared_ptr<TestHost> host;
  std::shared_ptr<policy::BuiltinPolicy> builtin;
  std::shared_ptr<policy::LuaPolicy> lua;
  std::shared_ptr<types::Subscriber> alice;
  std::shared_ptr<types::Subscriber> bob;

  EquivalenceFixture() {
    host = std::make_shared<TestHost>(core);
    builtin = std::make_shared<policy::BuiltinPolicy>(logger, nullptr);
    lua = std::make_shared<policy::LuaPolicy>(logger, nullptr);
    builtin->attach(host);
    lua->attach(host);

    auto realm = seed_realm("example.com");
    realm->registration_minimum = 60;
    realm->behaviour.media_profile = types::MediaPolicy::Profiles::WebRtc;
    realm->behaviour.qualify_interval = 30;
    store->realm_update(realm);

    alice = seed_subscriber(1, "sip:alice@example.com", "alice-ha1");
    bob = seed_subscriber(2, "sip:bob@example.com", "bob-ha1");

    auto phone = make_channel("192.0.2.10");
    register_binding(alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060"), phone, 3600);

    on_strand([this]() { core->local_address_add("192.0.2.1:5060"); });
    config->behaviour.anchor = false;
  }

  static std::shared_ptr<policy::RequestView> view(const std::string& raw) {
    auto out = std::make_shared<policy::RequestView>();
    out->message = std::make_shared<SIPMessage>();
    out->message->header = std::make_shared<SIPHeader>(raw);
    out->has_route = out->message->header->contains("Route");
    return out;
  }

  template <typename T, typename Ask>
  plugins::Result<T> ask(Ask ask) {
    std::promise<plugins::Result<T>> promise;
    auto future = promise.get_future();
    core->post([&]() { ask(core->strand(), [&promise](plugins::Result<T> result) { promise.set_value(std::move(result)); }); });
    return future.get();
  }

  // The two drivers' answers to one question, described for comparison.
  std::pair<std::string, std::string> authorize(const std::shared_ptr<policy::RequestView>& request) {
    auto first = ask<policy::AuthDecision>([&](plugins::Executor on, plugins::Handler<policy::AuthDecision> h) { builtin->authorize(on, request, h); });
    auto second = ask<policy::AuthDecision>([&](plugins::Executor on, plugins::Handler<policy::AuthDecision> h) { lua->authorize(on, request, h); });
    return {describe(first), describe(second)};
  }

  std::pair<std::string, std::string> route(const std::shared_ptr<policy::RequestView>& request) {
    auto first = ask<policy::RouteDecision>([&](plugins::Executor on, plugins::Handler<policy::RouteDecision> h) { builtin->route(on, request, h); });
    auto second = ask<policy::RouteDecision>([&](plugins::Executor on, plugins::Handler<policy::RouteDecision> h) { lua->route(on, request, h); });
    return {describe(first), describe(second)};
  }

  std::pair<std::string, std::string> register_(const std::shared_ptr<policy::RequestView>& request) {
    auto first = ask<policy::RegisterDecision>([&](plugins::Executor on, plugins::Handler<policy::RegisterDecision> h) { builtin->register_(on, request, h); });
    auto second = ask<policy::RegisterDecision>([&](plugins::Executor on, plugins::Handler<policy::RegisterDecision> h) { lua->register_(on, request, h); });
    return {describe(first), describe(second)};
  }
};

std::string request(const std::string& method, const std::string& uri, const std::string& from, const std::string& to, const std::string& extra = "") {
  std::string raw = method + " " + uri + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-eq\r\n";
  if (!from.empty()) raw += "From: <" + from + ">;tag=f\r\n";
  if (!to.empty()) raw += "To: <" + to + ">\r\n";
  raw += "Call-ID: equivalence\r\nCSeq: 1 " + method + "\r\nMax-Forwards: 70\r\n" + extra;
  return raw;
}

#define EXPECT_SAME(pair) EXPECT_EQ((pair).first, (pair).second)

}  // namespace

TEST(PolicyEquivalenceTest, AuthorizeAgreesOnEveryBranch) {
  EquivalenceFixture f;

  for (const auto* method : {"ACK", "CANCEL"})
    EXPECT_SAME(f.authorize(EquivalenceFixture::view(request(method, "sip:bob@example.com", "sip:x@else.net", "sip:bob@example.com"))));

  auto peer = EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:x@else.net", "sip:bob@example.com"));
  peer->from_peer = true;
  EXPECT_SAME(f.authorize(peer));

  auto in_dialog = EquivalenceFixture::view(request("BYE", "sip:bob@192.0.2.20", "sip:x@else.net", "") + "To: <sip:bob@example.com>;tag=b\r\n");
  in_dialog->message->in_known_dialog = true;
  EXPECT_SAME(f.authorize(in_dialog));

  // A To tag in no known dialog proves nothing.
  in_dialog->message->in_known_dialog = false;
  EXPECT_SAME(f.authorize(in_dialog));

  auto token = EquivalenceFixture::view(request("BYE", "sip:bob@192.0.2.20", "sip:x@else.net", "sip:bob@example.com"));
  token->valid_flow_token = true;
  EXPECT_SAME(f.authorize(token));

  auto relay = EquivalenceFixture::view(request("REGISTER", "sip:else.net", "sip:x@else.net", "sip:x@else.net"));
  relay->relay = true;
  EXPECT_SAME(f.authorize(relay));

  EXPECT_SAME(f.authorize(EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "", "sip:bob@example.com"))));
  EXPECT_SAME(f.authorize(EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:alice@EXAMPLE.com", "sip:bob@example.com"))));
  EXPECT_SAME(f.authorize(EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:x@else.net", "sip:bob@example.com"))));
  EXPECT_SAME(f.authorize(EquivalenceFixture::view(request("INVITE", "sip:y@other.net", "sip:x@else.net", "sip:y@other.net"))));
  EXPECT_SAME(f.authorize(
      EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:x@else.net", "sip:bob@example.com", "Route: <sip:proxy.else.net;lr>\r\n"))));
}

TEST(PolicyEquivalenceTest, RouteAgreesOnEveryBranch) {
  EquivalenceFixture f;

  EXPECT_SAME(f.route(EquivalenceFixture::view(request("INVITE", "sip:alice@example.com", "sip:x@else.net", "sip:alice@example.com"))));
  EXPECT_SAME(f.route(EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:x@else.net", "sip:bob@example.com"))));
  EXPECT_SAME(f.route(EquivalenceFixture::view(request("INVITE", "sip:carol@example.com", "sip:x@else.net", "sip:carol@example.com"))));
  EXPECT_SAME(f.route(EquivalenceFixture::view(request("INVITE", "sip:y@other.net", "sip:alice@example.com", "sip:y@other.net"))));
  EXPECT_SAME(f.route(EquivalenceFixture::view(request("MESSAGE", "sip:alice@Example.COM", "sip:x@else.net", "sip:alice@example.com"))));

  // The off-node answer carries the server's media policy, which the fixture turned anchoring off in.
  const auto off_node = f.route(EquivalenceFixture::view(request("INVITE", "sip:y@other.net", "sip:alice@example.com", "sip:y@other.net")));
  EXPECT_NE(off_node.first.find("media=no-anchor"), std::string::npos) << off_node.first;
}

TEST(PolicyEquivalenceTest, RegisterAgreesOnEveryBranch) {
  EquivalenceFixture f;

  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:example.com", "sip:alice@example.com", "sip:alice@example.com"))));
  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:example.com", "sip:alice@example.com", ""))));
  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:192.0.2.1", "sip:x@else.net", "sip:x@else.net"))));
  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:example.com", "sip:x@else.net", "sip:x@else.net"))));
  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:else.net", "sip:x@else.net", "sip:x@else.net"))));

  f.config->sip_forward_register = "never";
  EXPECT_SAME(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:else.net", "sip:x@else.net", "sip:x@else.net"))));
}

// A store that cannot answer is a failure from both, never a decision made without it.
TEST(PolicyEquivalenceTest, BothFailWhenTheStoreFails) {
  EquivalenceFixture f;
  f.host->down = true;

  const auto invite = EquivalenceFixture::view(request("INVITE", "sip:bob@example.com", "sip:alice@example.com", "sip:bob@example.com"));
  EXPECT_EQ(f.authorize(invite), std::make_pair(std::string("failed"), std::string("failed")));
  EXPECT_EQ(f.route(invite), std::make_pair(std::string("failed"), std::string("failed")));
  EXPECT_EQ(f.register_(EquivalenceFixture::view(request("REGISTER", "sip:example.com", "sip:alice@example.com", "sip:alice@example.com"))),
            std::make_pair(std::string("failed"), std::string("failed")));
}

// The whole of the proxy and registrar tests, again, with the standard scripts in place of builtin://. They were
// written against builtin://, so passing them is behaving as it does.
TEST(PolicyEquivalenceTest, TheProxyAndRegistrarTestsPassUnderTheStandardScripts) {
  if (const char* policy = std::getenv("ATHENA_TEST_POLICY"); policy != nullptr) GTEST_SKIP() << "already running under ATHENA_TEST_POLICY";

  const std::string command = std::string("ATHENA_TEST_POLICY=lua '") + ATHENA_TEST_BINARY +
                              "' --gtest_brief=1 --gtest_filter='Proxy*:Registrar*:LocalUa*:Core*:Dialogs*:Calls*' > /dev/null 2>&1";
  EXPECT_EQ(std::system(command.c_str()), 0) << "run: " << command;
}

// A script routes out by a trunk with athenasip.route.trunk, and trusts a request in from one with auth.trusted.
TEST(LuaPolicyTest, AScriptRoutesOutByATrunkAndTrustsOneIn) {
  EquivalenceFixture f;

  auto acme = std::make_shared<types::Trunk>();
  acme->name = "acme";
  acme->uri = "sip:sip.acme.example;transport=tls";
  ASSERT_TRUE(f.store->trunk_create(acme));

  const auto scripts = std::filesystem::temp_directory_path() / ("athenasip-trunk-" + std::to_string(::getpid()));
  std::filesystem::create_directories(scripts);
  std::ofstream(scripts / "main.lua") << R"(
    function authorize(request) return athenasip.auth.trusted{trunk = "acme"} end
    function route(request)
      return athenasip.route.forward({athenasip.route.trunk(athenasip.store.trunk("acme"), {user = request.uri.user})})
    end
    function register(request) return athenasip.register.reject(403) end
  )";

  auto lua = std::make_shared<policy::LuaPolicy>(f.logger, nullptr);
  YAML::Node own;
  own["path"].push_back(scripts.string());
  ASSERT_TRUE(lua->configure(own, *f.config)) << lua->error();
  lua->attach(f.host);
  std::filesystem::remove_all(scripts);

  const auto invite = EquivalenceFixture::view(request("INVITE", "sip:+442071234567@example.com", "sip:alice@example.com", "sip:+442071234567@example.com"));

  const auto auth = f.ask<policy::AuthDecision>([&](plugins::Executor on, plugins::Handler<policy::AuthDecision> h) { lua->authorize(on, invite, h); });
  ASSERT_TRUE(auth.ok) << auth.error;
  EXPECT_EQ(auth.value.kind, policy::AuthDecision::Kind::Trusted);
  EXPECT_EQ(auth.value.trunk, "acme");

  const auto route = f.ask<policy::RouteDecision>([&](plugins::Executor on, plugins::Handler<policy::RouteDecision> h) { lua->route(on, invite, h); });
  ASSERT_TRUE(route.ok) << route.error;
  ASSERT_EQ(route.value.targets.size(), 1u);
  EXPECT_EQ(route.value.targets[0].trunk, "acme");
  EXPECT_EQ(route.value.targets[0].uri->to_string(), "sip:+442071234567@sip.acme.example;transport=tls");
}
