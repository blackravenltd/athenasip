//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <set>
#include <string>

#include "events/topics.h"
#include "helpers/proxy_fixture_helper.h"
#include "notifier.h"

using namespace athenasip;

namespace {

// Carol, at reception, watches the lines of the call Alice makes to Bob (RFC 4235): Bob's as the recipient, Alice's
// as the initiator. Carol is in the same realm and proven by her REGISTER.
struct DialogPackageFixture : ProxyFixture {
  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;
  int subscriptions = 0;

  DialogPackageFixture() {
    bind_bob();
    seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    carol = make_channel("192.0.2.30", &carol_connection);
    on_strand([this]() { carol->authenticated_as("sip:carol@example.com"); });
  }

  void watch(const std::string& presentity) {
    ++subscriptions;
    std::string raw = "SUBSCRIBE " + presentity + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "To: <" + presentity + ">\r\n";
    raw += "Call-ID: blf-" + std::to_string(subscriptions) + "\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: dialog\r\n";
    raw += "Expires: 600\r\n";
    raw += "\r\n";
    receive(carol, raw);
    answer_notifies();
  }

  // Carol's phone answers every NOTIFY it is sent, as a phone does.
  void answer_notifies() {
    for (const auto& notify : requests_with(carol_connection, "NOTIFY")) {
      const auto cseq = notify->header->headers_map["CSeq"][0]->to_string();
      const auto key = notify->header->headers_map["Call-ID"][0]->to_string() + cseq;
      if (_answered.count(key)) continue;
      _answered.insert(key);

      std::string raw = "SIP/2.0 200 OK\r\n";
      for (const auto& via : notify->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
      raw += "From: " + notify->header->headers_map["From"][0]->to_string() + "\r\n";
      raw += "To: " + notify->header->headers_map["To"][0]->to_string() + "\r\n";
      raw += "Call-ID: " + notify->header->headers_map["Call-ID"][0]->to_string() + "\r\n";
      raw += "CSeq: " + cseq + "\r\n";
      raw += "\r\n";
      receive(carol, raw);
    }
  }

  // The latest document Carol holds for one of her subscriptions.
  std::string latest(int subscription = 1) {
    answer_notifies();
    std::string body;
    for (const auto& notify : requests_with(carol_connection, "NOTIFY")) {
      if (notify->header->headers_map["Call-ID"][0]->to_string() == "blf-" + std::to_string(subscription)) body = notify->body;
    }
    return body;
  }

  std::string bye_from_alice() {
    auto forwarded = request_with(callee_connection, "INVITE");
    std::string raw = "BYE sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-bye\r\n";
    raw += "Max-Forwards: 70\r\n";
    for (const auto& route : forwarded->header->headers_map["Record-Route"]) raw += "Route: " + route->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=bob\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 2 BYE\r\n";
    raw += "\r\n";
    return raw;
  }

 private:
  std::set<std::string> _answered;
};

}  // namespace

// The proxy says who each leg of a call is, and the record carries it: the caller it proved, and the callee whose
// binding it rang.
TEST(DialogPackageTest, TheCallRecordSaysWhichSubscriberEachLegIs) {
  DialogPackageFixture f;
  f.receive(f.caller, f.invite());

  auto record = f.store->call_get("call-proxy");
  ASSERT_NE(record, nullptr);
  ASSERT_EQ(record->participants.size(), 2u);
  EXPECT_EQ(record->participants[0].subscriber, "sip:alice@example.com");
  EXPECT_EQ(record->participants[1].subscriber, "sip:bob@example.com");
  EXPECT_EQ(f.store->call_list_for_subscriber("sip:bob@example.com").size(), 1u);
}

// A caller nobody proved, calling into the realm from outside it, is no subscriber's line.
TEST(DialogPackageTest, AStrangersLegIsNobodys) {
  DialogPackageFixture f;
  std::shared_ptr<MockConnection> stranger_connection;
  auto stranger = f.make_channel("192.0.2.40", &stranger_connection);

  auto raw = f.invite();
  raw.replace(raw.find("From: <sip:alice@example.com>"), std::string("From: <sip:alice@example.com>").size(), "From: <sip:dave@elsewhere.example>");
  f.receive(stranger, raw);

  auto record = f.store->call_get("call-proxy");
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->participants[0].subscriber, "");
  EXPECT_EQ(record->participants[1].subscriber, "sip:bob@example.com");
}

// RFC 4235 3.7.1: the recipient's line, through a call: early while it rings, confirmed once answered, and gone from
// the full document once it ends.
TEST(DialogPackageTest, TheCalleesLineRingsThenIsInUseThenIsFree) {
  DialogPackageFixture f;
  f.watch("sip:bob@example.com");
  EXPECT_EQ(f.latest().find("<dialog "), std::string::npos) << "idle: no dialogs";

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  auto ringing = f.latest();
  EXPECT_NE(ringing.find("direction=\"recipient\""), std::string::npos) << ringing;
  EXPECT_NE(ringing.find("<state>early</state>"), std::string::npos) << ringing;

  f.receive(f.callee, f.response_from_callee(200, "OK"));
  auto answered = f.latest();
  EXPECT_NE(answered.find("<state>confirmed</state>"), std::string::npos) << answered;

  f.receive(f.caller, f.bye_from_alice());
  auto ended = f.latest();
  EXPECT_EQ(ended.find("<dialog "), std::string::npos) << ended;
}

// The initiator's line shows as soon as it calls (RFC 4235 3.7.1: trying).
TEST(DialogPackageTest, TheCallersLineShowsAsSoonAsItCalls) {
  DialogPackageFixture f;
  f.watch("sip:alice@example.com");

  f.receive(f.caller, f.invite());
  auto trying = f.latest();
  EXPECT_NE(trying.find("direction=\"initiator\""), std::string::npos) << trying;
  EXPECT_NE(trying.find("<state>trying</state>"), std::string::npos) << trying;
}

// RFC 4235 4.1 and RFC 3891: each dialog names its Call-ID and tags, which a phone needs to pick the call up, and the
// identities of both ends.
TEST(DialogPackageTest, ADialogCarriesWhatAPickupNeeds) {
  DialogPackageFixture f;
  f.watch("sip:bob@example.com");
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  auto body = f.latest();
  EXPECT_NE(body.find("call-id=\"call-proxy\""), std::string::npos) << body;
  EXPECT_NE(body.find("local-tag=\"bob\""), std::string::npos) << body;
  EXPECT_NE(body.find("remote-tag=\"alice\""), std::string::npos) << body;
  EXPECT_NE(body.find("<local><identity>sip:bob@example.com</identity></local>"), std::string::npos) << body;
  EXPECT_NE(body.find("<remote><identity>sip:alice@example.com</identity></remote>"), std::string::npos) << body;
}

// RFC 4235 4.1: every document on a subscription has a version one higher than the last.
TEST(DialogPackageTest, EachDocumentsVersionRises) {
  DialogPackageFixture f;
  f.watch("sip:bob@example.com");
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.answer_notifies();

  int expected = 0;
  for (const auto& notify : f.requests_with(f.carol_connection, "NOTIFY")) {
    EXPECT_NE(notify->body.find("version=\"" + std::to_string(expected) + "\""), std::string::npos) << notify->body;
    ++expected;
  }
  EXPECT_GE(expected, 3);
}

// RFC 6665 4.2.2: a NOTIFY says the state changed. A call record written while the callee is not yet reached, or a
// signal with nothing behind it, leaves Bob's line as Carol last saw it, so she is sent nothing.
TEST(DialogPackageTest, AWriteThatLeavesTheLineAsItWasSendsNothing) {
  DialogPackageFixture f;
  f.watch("sip:bob@example.com");
  ASSERT_EQ(f.requests_with(f.carol_connection, "NOTIFY").size(), 1u);

  f.receive(f.caller, f.invite());
  f.answer_notifies();
  EXPECT_EQ(f.requests_with(f.carol_connection, "NOTIFY").size(), 1u) << "a callee not yet reached has no dialog to report";

  f.on_strand([&f]() { f.core->notifier()->changed("sip:bob@example.com"); });
  f.settle();
  f.answer_notifies();
  EXPECT_EQ(f.requests_with(f.carol_connection, "NOTIFY").size(), 1u);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  EXPECT_NE(f.latest().find("<state>early</state>"), std::string::npos) << "a change is still sent";
  EXPECT_EQ(f.requests_with(f.carol_connection, "NOTIFY").size(), 2u);
}

// A watcher of somebody else hears nothing of this call.
TEST(DialogPackageTest, AnotherSubscribersWatcherHearsNothing) {
  DialogPackageFixture f;
  f.watch("sip:carol@example.com");
  const auto before = f.requests_with(f.carol_connection, "NOTIFY").size();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  f.answer_notifies();

  EXPECT_EQ(f.requests_with(f.carol_connection, "NOTIFY").size(), before);
}

// The bus carries an AOR in a topic, and a subscriber may be named "+4420...": MQTT forbids + and # in a topic name.
TEST(DialogPackageTest, AnAorTravelsTheBusIntact) {
  const std::string aor = "sip:+44 20#7946/0000@example.com";
  const auto topic = events::topics::presence_changed(aor);
  EXPECT_EQ(topic.find('+'), std::string::npos);
  EXPECT_EQ(topic.find('#'), std::string::npos);
  EXPECT_EQ(std::count(topic.begin(), topic.end(), '/'), 2);
  EXPECT_EQ(events::topics::presence_changed_aor(topic), aor);
  EXPECT_EQ(events::topics::presence_changed_aor("nodes/a/status"), "");
}

// A BLF key for a conference room (docs/design/conferencing.md): the room is a presentity of its own, whose line
// is in use while anyone's call to its focus is up.
TEST(DialogPackageTest, AKeyForARoomLightsWhileAnyoneIsInIt) {
  DialogPackageFixture f;
  f.watch("sip:conf-sales@example.com");
  EXPECT_EQ(f.latest().find("<dialog "), std::string::npos) << "an empty room";

  f.on_strand([&f]() {
    auto call = std::make_shared<Call>();
    call->id = "call-room";
    call->state = Call::State::Connected;
    call->answered_at = std::time(nullptr);
    call->focus = std::make_shared<types::SIPUri>("sip:conf-sales@example.com");
    auto& alice = call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
    alice.subscriber = "sip:alice@example.com";
    auto& room = call->add_participant(std::make_shared<types::SIPIdentity>("sip:conf-sales@192.0.2.40:5060"));
    room.trunk = "rooms";
    f.core->call_register(call);
  });
  f.settle();

  const auto calls = f.on_strand([&f]() { return f.core->call_list(); });
  const auto dialogs = Notifier::dialogs_of("sip:conf-sales@example.com", calls);
  ASSERT_EQ(dialogs.size(), 1u);
  EXPECT_EQ(dialogs[0].state, "confirmed");
  EXPECT_EQ(dialogs[0].direction, "recipient");
  EXPECT_EQ(dialogs[0].local, "sip:conf-sales@example.com");
  EXPECT_EQ(dialogs[0].remote, "sip:alice@example.com");

  EXPECT_TRUE(Notifier::dialogs_of("sip:conf-other@example.com", f.on_strand([&f]() { return f.core->call_list(); })).empty());
}
