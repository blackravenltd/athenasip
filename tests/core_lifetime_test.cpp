//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// What a node holds it lets go of: a call once it has ended, a flow once it has closed, and the whole Core once
// it has been stopped and dropped. A reference cycle anywhere among them is memory a running node never gets back.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

std::string bye_from_caller() {
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

std::string ok_to_bye(const std::shared_ptr<athenasip::SIPMessage>& bye) {
  std::string raw = "SIP/2.0 200 OK\r\n";
  for (const auto& via : bye->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>;tag=bob\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: 2 BYE\r\n\r\n";
  return raw;
}

// Past timer K and timer D (RFC 3261 17.1.2.2, 17.1.1.2) and every server transaction's wait, so no transaction
// of the call is left holding anything.
constexpr auto kAllTransactionsDone = std::chrono::seconds(64);

}  // namespace

TEST(CoreLifetimeTest, ACoreDroppedIsFreed) {
  std::weak_ptr<Core> core;
  {
    CoreFixture f;
    core = f.core;
  }
  EXPECT_TRUE(core.expired());
}

TEST(CoreLifetimeTest, ACoreWithFlowsIsFreedWhenDropped) {
  std::weak_ptr<Core> core;
  std::weak_ptr<Channel> caller;
  {
    ProxyFixture f;
    f.bind_bob();
    core = f.core;
    caller = f.caller;
  }
  EXPECT_TRUE(core.expired());
  EXPECT_TRUE(caller.expired());
}

TEST(CoreLifetimeTest, ACoreThatCarriedACallIsFreedWhenDropped) {
  std::weak_ptr<Core> core;
  {
    ProxyFixture f;
    f.bind_bob();
    core = f.core;

    f.receive(f.caller, f.invite());
    f.receive(f.callee, f.response_from_callee(200, "OK"));
    f.receive(f.caller, bye_from_caller());
  }
  EXPECT_TRUE(core.expired());
}

// A node stopped with a call still up, its transactions unfinished, lets go of all of it.
TEST(CoreLifetimeTest, ANodeStoppedMidCallLetsGoOfTheCall) {
  std::weak_ptr<SIPMessage> invite;
  {
    ProxyFixture f;
    f.bind_bob();

    const auto raw = f.invite();
    auto message = std::make_shared<SIPMessage>();
    message->header = std::make_shared<SIPHeader>(raw.substr(0, raw.find("\r\n\r\n")));
    invite = message;
    f.on_strand([&]() { f.caller->receive(message); });
    f.settle();
    message.reset();

    f.receive(f.callee, f.response_from_callee(200, "OK"));
  }
  EXPECT_TRUE(invite.expired()) << "a stopped node still holds the INVITE of the call it was carrying";
}

// The same with a request forwarded and not yet answered: its client transaction is open when the node stops.
TEST(CoreLifetimeTest, ANodeStoppedWithARequestUnansweredLetsGoOfIt) {
  std::weak_ptr<SIPMessage> bye;
  {
    ProxyFixture f;
    f.bind_bob();
    f.receive(f.caller, f.invite());
    f.receive(f.callee, f.response_from_callee(200, "OK"));

    const auto raw = bye_from_caller();
    auto message = std::make_shared<SIPMessage>();
    message->header = std::make_shared<SIPHeader>(raw.substr(0, raw.find("\r\n\r\n")));
    bye = message;
    f.on_strand([&]() { f.caller->receive(message); });
    f.settle();
    message.reset();

    ASSERT_NE(ProxyFixture::request_with(f.callee_connection, "BYE"), nullptr);
  }
  EXPECT_TRUE(bye.expired()) << "a stopped node still holds a request it forwarded and had no answer to";
}

// The running node, not a teardown: a call that has ended and whose transactions are over is gone from memory.
TEST(CoreLifetimeTest, ARunningNodeLetsGoOfACallThatEnded) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));

  std::weak_ptr<Call> call = f.call();
  ASSERT_FALSE(call.expired());

  f.receive(f.caller, bye_from_caller());
  const auto bye = ProxyFixture::request_with(f.callee_connection, "BYE");
  ASSERT_NE(bye, nullptr);
  f.receive(f.callee, ok_to_bye(bye));

  f.advance(kAllTransactionsDone);
  f.settle();

  EXPECT_EQ(f.call(), nullptr);

  // The record stays in the datastore for its retention (memory://'s is that same object); nothing else may hold it.
  auto held = call.lock();
  ASSERT_NE(held, nullptr);
  ASSERT_EQ(f.store->call_get("call-proxy").get(), held.get());
  EXPECT_EQ(held.use_count(), 2) << "something besides the call record still holds the call after it ended";
  EXPECT_EQ(f.on_strand([&f]() { return f.core->transaction_count(); }), 0u);
}

// The requests of a call are held by its transactions and the proxy's context for them, which a transaction's
// callbacks reach back to. A transaction that has terminated is destroyed (RFC 3261 17), so once the call is over
// and its transactions are done, nothing of it is left: not the INVITE the caller sent.
TEST(CoreLifetimeTest, ARunningNodeLetsGoOfTheRequestsOfACallThatEnded) {
  // Under the scripts a request handed to a hook is a Lua value, freed when Lua's collector next runs: later, not
  // never (a full collection after each hook frees it), and on no schedule a test can see.
  if (CoreFixture::lua_policy()) GTEST_SKIP() << "the request is Lua's to collect";

  ProxyFixture f;
  f.bind_bob();

  std::weak_ptr<SIPMessage> invite;
  {
    const auto raw = f.invite();
    const auto split = raw.find("\r\n\r\n");
    auto message = std::make_shared<SIPMessage>();
    message->header = std::make_shared<SIPHeader>(raw.substr(0, split));
    invite = message;
    f.on_strand([&]() { f.caller->receive(message); });
    f.settle();
  }

  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.receive(f.caller, bye_from_caller());
  if (const auto bye = ProxyFixture::request_with(f.callee_connection, "BYE")) f.receive(f.callee, ok_to_bye(bye));

  f.advance(kAllTransactionsDone);
  f.settle();

  ASSERT_EQ(f.on_strand([&f]() { return f.core->transaction_count(); }), 0u);
  EXPECT_TRUE(invite.expired()) << "something still holds the INVITE after its call ended and its transactions finished";
}

// A flow that has closed is gone from memory, while the node carries on.
TEST(CoreLifetimeTest, ARunningNodeLetsGoOfAFlowThatClosed) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.receive(f.caller, bye_from_caller());
  if (const auto bye = ProxyFixture::request_with(f.callee_connection, "BYE")) f.receive(f.callee, ok_to_bye(bye));
  f.advance(kAllTransactionsDone);

  std::weak_ptr<Channel> caller = f.caller;
  f.caller->close();
  f.settle();
  f.caller.reset();
  f.caller_connection.reset();
  f.settle();

  EXPECT_TRUE(caller.expired()) << "something still holds a flow after it closed";
}
