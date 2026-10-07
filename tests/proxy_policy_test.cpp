//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "helpers/proxy_fixture_helper.h"
#include "policy/policy.h"

using namespace athenasip;

namespace {

// A policy whose answers each test writes. A hook left unset answers as builtin:// would not: it fails, so a test
// that reaches a hook it did not expect sees a 500.
class ScriptedPolicy final : public policy::Policy {
 public:
  std::function<plugins::Result<policy::AuthDecision>(const policy::RequestView&)> on_authorize;
  std::function<plugins::Result<policy::RouteDecision>(const policy::RequestView&)> on_route;
  std::function<plugins::Result<policy::FailureDecision>(const SIPMessage&, policy::ForkState)> on_branch_failed;
  std::function<plugins::Result<policy::RegisterDecision>(const policy::RequestView&)> on_register;

  std::vector<std::uint16_t> failures_seen;

  std::string name() const override { return "scripted"; }
  std::string version() const override { return "0.0.0"; }

  void authorize(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::AuthDecision> handler) override {
    _complete(on, handler, on_authorize ? on_authorize(*request) : plugins::Result<policy::AuthDecision>::failure("no authorize"));
  }

  void route(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::RouteDecision> handler) override {
    _complete(on, handler, on_route ? on_route(*request) : plugins::Result<policy::RouteDecision>::failure("no route"));
  }

  void on_failure(plugins::Executor on, std::shared_ptr<policy::RequestView> request, std::shared_ptr<SIPMessage> response, policy::ForkState state,
                  plugins::Handler<policy::FailureDecision> handler) override {
    (void)request;
    failures_seen.push_back(static_cast<std::uint16_t>(response->header->response_code));
    _complete(on, handler,
              on_branch_failed ? on_branch_failed(*response, state) : plugins::Result<policy::FailureDecision>::success(policy::FailureDecision::next()));
  }

  void register_(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::RegisterDecision> handler) override {
    _complete(on, handler, on_register ? on_register(*request) : plugins::Result<policy::RegisterDecision>::failure("no register"));
  }
};

template <typename T>
plugins::Result<T> decided(T decision) {
  return plugins::Result<T>::success(std::move(decision));
}

std::shared_ptr<types::SIPUri> uri(const std::string& text) { return std::make_shared<types::SIPUri>(text); }

struct PolicyFixture : ProxyFixture {
  std::shared_ptr<ScriptedPolicy> policy = std::make_shared<ScriptedPolicy>();

  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;

  PolicyFixture() {
    carol = make_channel("192.0.2.30", &carol_connection);
    on_strand([this]() { core->policy_register(policy); });
    policy->on_authorize = [](const policy::RequestView&) { return decided(policy::AuthDecision::accept()); };
  }

  // A final answer from Carol to the INVITE she was sent, back up its Via chain.
  std::string response_from_carol(int code, const std::string& reason) {
    auto forwarded = request_with(carol_connection, "INVITE");
    if (!forwarded) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=carol\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "\r\n";
    return raw;
  }

  std::string register_request() {
    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-register\r\n";
    raw += "From: <sip:alice@example.com>;tag=reg\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-register\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// The policy decides who may call; the node answers what it says and forwards nothing.
TEST(ProxyPolicyTest, ARequestThePolicyRefusesIsAnsweredWithItsCode) {
  PolicyFixture fixture;
  fixture.bind_bob();
  fixture.policy->on_authorize = [](const policy::RequestView&) { return decided(policy::AuthDecision::reject(603, "Decline")); };

  fixture.receive(fixture.caller, fixture.invite());

  auto answer = ProxyFixture::response_with(fixture.caller_connection, 603);
  ASSERT_NE(answer, nullptr);
  EXPECT_EQ(answer->header->response_message, "Decline");
  EXPECT_EQ(ProxyFixture::request_with(fixture.callee_connection, "INVITE"), nullptr);
}

// A policy that cannot decide - a store it read failed, a script raised - is the server failing: 500.
TEST(ProxyPolicyTest, APolicyThatCannotDecideIsAServerError) {
  PolicyFixture fixture;
  fixture.policy->on_authorize = [](const policy::RequestView&) { return plugins::Result<policy::AuthDecision>::failure("the store is down"); };

  fixture.receive(fixture.caller, fixture.invite());

  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 500), nullptr);
}

// The policy may answer in the node's place rather than name a target.
TEST(ProxyPolicyTest, ARouteThatRepliesSendsThatReply) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) { return decided(policy::RouteDecision::reply(486, "Busy Here")); };

  fixture.receive(fixture.caller, fixture.invite());

  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 486), nullptr);
  EXPECT_EQ(ProxyFixture::request_with(fixture.callee_connection, "INVITE"), nullptr);
}

// RFC 3261 16.5: the target set is the policy's. The Request-URI of the branch is the target's (16.6 step 2).
TEST(ProxyPolicyTest, ARequestGoesToTheTargetThePolicyNames) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) {
    return decided(policy::RouteDecision::forward({policy::Target::to(uri("sip:carol@192.0.2.30:5060"))}));
  };

  fixture.receive(fixture.caller, fixture.invite("z9hG4bK-invite", "sip:helpdesk@example.com"));

  auto forwarded = ProxyFixture::request_with(fixture.carol_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->to_string(), "sip:carol@192.0.2.30:5060");
}

// A route with nothing in it reaches nobody.
TEST(ProxyPolicyTest, ARouteWithNoTargetsIsTemporarilyUnavailable) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) { return decided(policy::RouteDecision::forward({})); };

  fixture.receive(fixture.caller, fixture.invite());

  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 480), nullptr);
}

// RFC 3261 16.7 step 4 leaves it to the proxy whether to try another branch. A policy that stops sends the best
// response so far.
TEST(ProxyPolicyTest, APolicyThatStopsAfterAFailureTriesNoMoreTargets) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) {
    return decided(policy::RouteDecision::forward({policy::Target::to(uri("sip:bob@192.0.2.20:5060")), policy::Target::to(uri("sip:carol@192.0.2.30:5060"))}));
  };
  fixture.policy->on_branch_failed = [](const SIPMessage&, policy::ForkState state) {
    EXPECT_EQ(state.tried, 1u);
    EXPECT_EQ(state.remaining, 1u);
    return decided(policy::FailureDecision::stop());
  };

  fixture.receive(fixture.caller, fixture.invite());
  fixture.receive(fixture.callee, fixture.response_from_callee(486, "Busy Here"));

  EXPECT_EQ(fixture.policy->failures_seen, std::vector<std::uint16_t>{486});
  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 486), nullptr);
  EXPECT_EQ(ProxyFixture::request_with(fixture.carol_connection, "INVITE"), nullptr);
}

// After a failure the policy may add targets, tried before the ones that remain.
TEST(ProxyPolicyTest, TargetsAddedAfterAFailureAreTriedNext) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) {
    return decided(policy::RouteDecision::forward({policy::Target::to(uri("sip:bob@192.0.2.20:5060"))}));
  };
  fixture.policy->on_branch_failed = [](const SIPMessage& response, policy::ForkState) {
    if (response.header->response_code == 486) return decided(policy::FailureDecision::next({policy::Target::to(uri("sip:carol@192.0.2.30:5060"))}));
    return decided(policy::FailureDecision::next());
  };

  fixture.receive(fixture.caller, fixture.invite());
  fixture.receive(fixture.callee, fixture.response_from_callee(486, "Busy Here"));

  auto forwarded = ProxyFixture::request_with(fixture.carol_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->to_string(), "sip:carol@192.0.2.30:5060");

  // Carol is the last branch; her 404 is lower than Bob's 486 and is the best response (16.7 step 6).
  fixture.receive(fixture.carol, fixture.response_from_carol(404, "Not Found"));
  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 404), nullptr);
}

// A 2xx ends the fork whatever the policy would say (RFC 3261 16.7 step 5): it is never asked.
TEST(ProxyPolicyTest, ThePolicyIsNotAskedAfterASuccess) {
  PolicyFixture fixture;
  fixture.policy->on_route = [](const policy::RequestView&) {
    return decided(policy::RouteDecision::forward({policy::Target::to(uri("sip:bob@192.0.2.20:5060"))}));
  };

  fixture.receive(fixture.caller, fixture.invite());
  fixture.receive(fixture.callee, fixture.response_from_callee(200, "OK"));

  EXPECT_TRUE(fixture.policy->failures_seen.empty());
  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 200), nullptr);
}

// The registrar asks the policy before it challenges; a refusal is final.
TEST(ProxyPolicyTest, ARegisterThePolicyRefusesIsNotChallenged) {
  PolicyFixture fixture;
  fixture.policy->on_register = [](const policy::RequestView&) { return decided(policy::RegisterDecision::reject(403, "Forbidden")); };

  fixture.receive(fixture.caller, fixture.register_request());

  EXPECT_NE(ProxyFixture::response_with(fixture.caller_connection, 403), nullptr);
  EXPECT_EQ(ProxyFixture::response_with(fixture.caller_connection, 401), nullptr);
}

// An accepted REGISTER is challenged in the realm the policy names.
TEST(ProxyPolicyTest, ARegisterThePolicyAcceptsIsChallengedInItsRealm) {
  PolicyFixture fixture;
  auto realm = fixture.store->realm_get_by_name("example.com");
  fixture.policy->on_register = [realm](const policy::RequestView&) { return decided(policy::RegisterDecision::accept(realm, 600, 0, 0)); };

  fixture.receive(fixture.caller, fixture.register_request());

  auto challenge = ProxyFixture::response_with(fixture.caller_connection, 401);
  ASSERT_NE(challenge, nullptr);
  ASSERT_TRUE(challenge->header->contains("WWW-Authenticate"));
  EXPECT_NE(challenge->header->headers_map["WWW-Authenticate"][0]->to_string().find("realm=\"example.com\""), std::string::npos);
}
