//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "helpers/fake_push_service_helper.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

const std::string kParameters = ";pn-provider=acme;pn-param=acme-param;pn-prid=bob-token";

// Bob's phone registered for push (RFC 8599 4.1.1) and then went to sleep: its flow is gone.
struct PushFixture : ProxyFixture {
  std::shared_ptr<FakePushService> acme = std::make_shared<FakePushService>("acme");
  std::shared_ptr<types::SIPUri> asleep = std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060" + kParameters);

  PushFixture() {
    on_strand([this]() { core->push_register(acme); });
    await_on_strand([this](plugins::StatusHandler handler) { core->subscriber_register(bob, asleep, nullptr, 3600, "", std::move(handler), "", 0, true); });
  }

  // The woken phone registers again over a new connection, through the registrar.
  std::shared_ptr<MockConnection> wake(const std::string& contact = "sip:bob@192.0.2.21:5070" + kParameters) {
    std::shared_ptr<MockConnection> connection;
    auto channel = make_channel("192.0.2.21", &connection, "tcp", 5070);
    auto uri = std::make_shared<types::SIPUri>(contact);

    await_on_strand([&](plugins::StatusHandler handler) { core->subscriber_register(bob, uri, channel, 3600, "", std::move(handler), "", 0, true); });
    on_strand([&]() { core->binding_registered(bob->id, uri); });
    settle();
    return connection;
  }

  std::string cancel() {
    std::string raw = "CANCEL sip:bob@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 CANCEL\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// RFC 8599 5.6.2: an INVITE for a push binding sends a push and holds the request; nothing goes to the client
// before it registers again.
TEST(ProxyPushTest, AnInviteForAPushBindingPushesAndWaits) {
  PushFixture f;

  f.receive(f.caller, f.invite());

  const auto sent = f.acme->sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].reason, push::Notification::Reason::Request);
  EXPECT_EQ(sent[0].prid, "bob-token");
  EXPECT_EQ(sent[0].param, "acme-param");

  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "INVITE").empty());
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 480), nullptr);
}

// RFC 8599 5.6.2: once the client has registered again, the request goes down the flow it registered on.
TEST(ProxyPushTest, TheHeldRequestGoesDownTheFlowTheClientRegisteredOn) {
  PushFixture f;
  f.receive(f.caller, f.invite());

  auto woken = f.wake();

  const auto forwarded = ProxyFixture::requests_with(woken, "INVITE");
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded[0]->header->request_uri->host, "192.0.2.21");
}

// RFC 8599 5.3: a registration whose pn-* parameters differ is another client's, and the request keeps waiting.
TEST(ProxyPushTest, ARegistrationWithOtherPushParametersDoesNotReleaseTheRequest) {
  PushFixture f;
  f.receive(f.caller, f.invite());

  auto other = f.wake("sip:bob@192.0.2.21:5070;pn-provider=acme;pn-param=acme-param;pn-prid=another-token");

  EXPECT_TRUE(ProxyFixture::requests_with(other, "INVITE").empty());
}

// A client registering through another node of the cluster is found in the store, without being told.
TEST(ProxyPushTest, ARegistrationTheProxyIsNotToldOfIsFoundInTheStore) {
  PushFixture f;
  f.receive(f.caller, f.invite());

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.21", &connection, "tcp", 5070);
  auto uri = std::make_shared<types::SIPUri>("sip:bob@192.0.2.21:5070" + kParameters);
  f.await_on_strand([&](plugins::StatusHandler handler) { f.core->subscriber_register(f.bob, uri, channel, 3600, "", std::move(handler), "", 0, true); });

  f.timers->advance(std::chrono::milliseconds(250));
  f.settle();

  EXPECT_EQ(ProxyFixture::requests_with(connection, "INVITE").size(), 1u);
}

// RFC 8599 5.6.2: when the push fails, the request is answered with an error, 480 being recommended.
TEST(ProxyPushTest, AFailedPushIs480) {
  PushFixture f;
  f.acme->fails = true;

  f.receive(f.caller, f.invite());

  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 480), nullptr);
}

// RFC 8599 5.6.2: when the bucket timer runs out before the client registers, the request is answered 480.
TEST(ProxyPushTest, AClientThatDoesNotWakeInTimeIs480) {
  PushFixture f;
  f.receive(f.caller, f.invite());

  for (int i = 0; i < 4 * static_cast<int>(f.config->push_timeout) - 1; ++i) {
    f.timers->advance(std::chrono::milliseconds(250));
    f.settle();
  }
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 480), nullptr);

  f.timers->advance(std::chrono::milliseconds(250));
  f.settle();
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 480), nullptr);
}

// RFC 3261 16.10: a CANCEL ends the wait; the caller gets 487 and the client, waking later, gets nothing.
TEST(ProxyPushTest, ACancelEndsTheWait) {
  PushFixture f;
  f.receive(f.caller, f.invite());

  f.receive(f.caller, f.cancel());
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 487), nullptr);

  auto woken = f.wake();
  EXPECT_TRUE(ProxyFixture::requests_with(woken, "INVITE").empty());
}

// A binding registered without push is forwarded to as before, push services or none.
TEST(ProxyPushTest, ABindingWithoutPushIsForwardedAtOnce) {
  ProxyFixture f;
  auto acme = std::make_shared<FakePushService>("acme");
  f.on_strand([&]() { f.core->push_register(acme); });
  f.bind_bob();

  f.receive(f.caller, f.invite());

  EXPECT_TRUE(acme->sent().empty());
  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
}
