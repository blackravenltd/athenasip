//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "notifier.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"
#include "types/presence_record.h"

using namespace athenasip;

namespace {

using Kind = types::PresenceRecord::Kind;

// Alice watches Bob's line (RFC 4235). Alice's connection carried her REGISTER, so she is proven as herself; Bob is
// registered on his.
struct NotifierFixture : ProxyFixture {
  NotifierFixture() { bind_bob(); }

  static std::string subscribe(const std::string& expires = "600", const std::string& event = "dialog", const std::string& branch = "z9hG4bK-sub1",
                               const std::string& from = "sip:alice@example.com", const std::string& to = "sip:bob@example.com",
                               const std::string& contact = "sip:alice@192.0.2.10:5060") {
    std::string raw = "SUBSCRIBE " + to + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <" + from + ">;tag=watcher\r\n";
    raw += "To: <" + to + ">\r\n";
    raw += "Call-ID: sub-1\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    if (!contact.empty()) raw += "Contact: <" + contact + ">\r\n";
    if (!event.empty()) raw += "Event: " + event + "\r\n";
    if (!expires.empty()) raw += "Expires: " + expires + "\r\n";
    raw += "Accept: application/dialog-info+xml\r\n";
    raw += "\r\n";
    return raw;
  }

  // A SUBSCRIBE inside the subscription's dialog: to the Contact the 200 gave, with both tags (RFC 6665 4.1.2.2).
  static std::string refresh(const std::shared_ptr<SIPMessage>& ok, const std::string& expires, int cseq, const std::string& branch,
                             const std::string& to_tag_override = "") {
    const auto target = value_of(ok, "Contact");
    const auto uri = target.substr(target.find('<') + 1, target.find('>') - target.find('<') - 1);
    const auto to_tag = to_tag_override.empty() ? tag(ok, "To") : to_tag_override;

    std::string raw = "SUBSCRIBE " + uri + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:alice@example.com>;tag=watcher\r\n";
    raw += "To: <sip:bob@example.com>;tag=" + to_tag + "\r\n";
    raw += "Call-ID: sub-1\r\n";
    raw += "CSeq: " + std::to_string(cseq) + " SUBSCRIBE\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw += "Event: dialog\r\n";
    raw += "Expires: " + expires + "\r\n";
    raw += "\r\n";
    return raw;
  }

  // The watcher's answer to a NOTIFY: its Via, tags and CSeq as received.
  static std::string answer(const std::shared_ptr<SIPMessage>& notify, int code = 200, const std::string& reason = "OK") {
    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : notify->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: " + value_of(notify, "From") + "\r\n";
    raw += "To: " + value_of(notify, "To") + "\r\n";
    raw += "Call-ID: " + value_of(notify, "Call-ID") + "\r\n";
    raw += "CSeq: " + value_of(notify, "CSeq") + "\r\n";
    raw += "\r\n";
    return raw;
  }

  std::vector<std::shared_ptr<SIPMessage>> notifies() { return requests_with(caller_connection, "NOTIFY"); }

  std::shared_ptr<SIPMessage> last_notify() {
    auto all = notifies();
    return all.empty() ? nullptr : all.back();
  }

  std::shared_ptr<SIPMessage> ok() { return response_with(caller_connection, 200); }

  void advance(std::chrono::seconds by) {
    on_strand([this, by]() { timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
    settle();
  }

  std::vector<types::PresenceRecord> subscriptions() { return store->presence_list("sip:bob@example.com", Kind::Subscription); }

  static std::string value_of(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    return message && message->header->contains(name) ? message->header->headers_map[name][0]->to_string() : "";
  }

  static std::string tag(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    const auto value = value_of(message, name);
    const auto at = value.find(";tag=");
    if (at == std::string::npos) return "";
    const auto end = value.find(';', at + 5);
    return value.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
  }

  static int cseq_of(const std::shared_ptr<SIPMessage>& message) { return std::stoi(value_of(message, "CSeq")); }
};

}  // namespace

// RFC 6665 4.2.1.1: an accepted SUBSCRIBE is answered 200 (not 202) with the granted Expires, and the node, as
// notifier, is now party to a dialog: its To tag and a Contact of its own.
TEST(NotifierTest, AnAcceptedSubscribeIsAnsweredByTheNodeWithTheGrantedExpiry) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());

  auto ok = f.ok();
  ASSERT_NE(ok, nullptr);
  EXPECT_EQ(f.response_with(f.caller_connection, 202), nullptr);
  EXPECT_EQ(NotifierFixture::value_of(ok, "Expires"), "600");
  EXPECT_FALSE(NotifierFixture::tag(ok, "To").empty());
  EXPECT_NE(NotifierFixture::value_of(ok, "Contact"), "");
  EXPECT_NE(NotifierFixture::value_of(ok, "Allow-Events").find("dialog"), std::string::npos) << "RFC 6665 7.2.2";

  EXPECT_TRUE(f.requests_with(f.callee_connection, "SUBSCRIBE").empty()) << "the node is the notifier; Bob's phone is not asked";
}

// RFC 6665 4.2.1.2: a NOTIFY follows at once, in the subscription's dialog, with the state in full. RFC 4235 4.1:
// a dialog-info document for the presentity, version 0, state full.
TEST(NotifierTest, AnImmediateNotifyCarriesTheStateInTheSubscriptionsDialog) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());

  auto ok = f.ok();
  auto notify = f.last_notify();
  ASSERT_NE(ok, nullptr);
  ASSERT_NE(notify, nullptr);

  EXPECT_EQ(notify->header->request_uri->to_string(), "sip:alice@192.0.2.10:5060") << "the watcher's Contact is the remote target";
  EXPECT_EQ(NotifierFixture::value_of(notify, "Call-ID"), "sub-1");
  EXPECT_EQ(NotifierFixture::tag(notify, "From"), NotifierFixture::tag(ok, "To")) << "the notifier's tag";
  EXPECT_EQ(NotifierFixture::tag(notify, "To"), "watcher");
  EXPECT_NE(NotifierFixture::value_of(notify, "From").find("sip:bob@example.com"), std::string::npos);
  EXPECT_EQ(NotifierFixture::value_of(notify, "Event"), "dialog");
  EXPECT_EQ(NotifierFixture::value_of(notify, "Subscription-State").rfind("active", 0), 0u);
  EXPECT_NE(NotifierFixture::value_of(notify, "Subscription-State").find("expires="), std::string::npos);
  EXPECT_FALSE(NotifierFixture::value_of(notify, "Max-Forwards").empty());
  EXPECT_FALSE(NotifierFixture::value_of(notify, "Contact").empty()) << "RFC 6665 4.2.2: a NOTIFY carries a Contact";

  EXPECT_EQ(NotifierFixture::value_of(notify, "Content-Type"), "application/dialog-info+xml");
  EXPECT_NE(notify->body.find("<dialog-info"), std::string::npos);
  EXPECT_NE(notify->body.find("version=\"0\""), std::string::npos);
  EXPECT_NE(notify->body.find("state=\"full\""), std::string::npos);
  EXPECT_NE(notify->body.find("entity=\"sip:bob@example.com\""), std::string::npos);
}

// The subscription is a record in the datastore, held by this node (docs/design/presence.md).
TEST(NotifierTest, TheSubscriptionIsARecordHeldByThisNode) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());

  auto records = f.subscriptions();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().node_id, "test-node");
  EXPECT_EQ(f.store->presence_list_node("test-node").size(), 1u);
}

// RFC 6665 4.2.1.1: the notifier may shorten the duration, never lengthen it; a SUBSCRIBE with none gets the
// package's default, 3600 for dialog (RFC 4235 3.3).
TEST(NotifierTest, TheExpiryIsCappedAndDefaulted) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("86400"));
  EXPECT_EQ(NotifierFixture::value_of(f.ok(), "Expires"), "3600");

  NotifierFixture g;
  g.receive(g.caller, NotifierFixture::subscribe(""));
  EXPECT_EQ(NotifierFixture::value_of(g.ok(), "Expires"), "3600");
}

// RFC 6665 4.2.1.1, RFC 3261 21.4.17: too short is 423 with the shortest acceptable, and nothing is created.
TEST(NotifierTest, ATooShortExpiryIs423WithMinExpires) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("10"));

  auto brief = f.response_with(f.caller_connection, 423);
  ASSERT_NE(brief, nullptr);
  EXPECT_EQ(NotifierFixture::value_of(brief, "Min-Expires"), "60");
  EXPECT_TRUE(f.subscriptions().empty());
  EXPECT_TRUE(f.notifies().empty());
}

// RFC 6665 4.1.2.1: a SUBSCRIBE must carry a Contact, which is the remote target of the dialog it makes.
TEST(NotifierTest, ASubscribeWithNoContactIs400) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("600", "dialog", "z9hG4bK-sub1", "sip:alice@example.com", "sip:bob@example.com", ""));
  EXPECT_NE(f.response_with(f.caller_connection, 400), nullptr);
  EXPECT_TRUE(f.subscriptions().empty());
}

// Nobody of that name in a realm served here.
TEST(NotifierTest, ASubscribeToNobodyIs404) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("600", "dialog", "z9hG4bK-sub1", "sip:alice@example.com", "sip:nobody@example.com"));
  EXPECT_NE(f.response_with(f.caller_connection, 404), nullptr);
  EXPECT_TRUE(f.notifies().empty());
}

// Decided 2026-10-11: watchers are the presentity's own realm. A stranger whom the policy lets call into the realm
// may not watch it.
TEST(NotifierTest, AWatcherFromOutsideTheRealmIs403) {
  NotifierFixture f;
  std::shared_ptr<MockConnection> stranger_connection;
  auto stranger = f.make_channel("192.0.2.30", &stranger_connection);

  f.receive(stranger,
            NotifierFixture::subscribe("600", "dialog", "z9hG4bK-sub1", "sip:carol@elsewhere.example", "sip:bob@example.com", "sip:carol@192.0.2.30:5060"));

  EXPECT_NE(NotifierFixture::response_with(stranger_connection, 403), nullptr);
  EXPECT_TRUE(f.subscriptions().empty());
}

// Another realm on the same node is still another realm.
TEST(NotifierTest, AWatcherProvenInAnotherRealmOnThisNodeIs403) {
  NotifierFixture f;
  auto other = f.seed_realm("other.example");
  (void)other;
  f.seed_subscriber(3, "sip:carol@other.example", "carol-ha1");

  std::shared_ptr<MockConnection> carol_connection;
  auto carol = f.make_channel("192.0.2.30", &carol_connection);
  f.on_strand([&carol]() { carol->authenticated_as("sip:carol@other.example"); });

  f.receive(carol, NotifierFixture::subscribe("600", "dialog", "z9hG4bK-sub1", "sip:carol@other.example", "sip:bob@example.com", "sip:carol@192.0.2.30:5060"));

  EXPECT_NE(NotifierFixture::response_with(carol_connection, 403), nullptr);
  EXPECT_TRUE(f.subscriptions().empty());
}

// Decided 2026-10-11: any package but the two is forwarded to the devices, as every SUBSCRIBE was.
TEST(NotifierTest, AnotherPackageIsForwardedToThePresentitysDevices) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("600", "message-summary"));

  EXPECT_EQ(f.requests_with(f.callee_connection, "SUBSCRIBE").size(), 1u);
  EXPECT_TRUE(f.notifies().empty());
  EXPECT_TRUE(f.subscriptions().empty());
}

// presence.enable: false puts things back as they were.
TEST(NotifierTest, WithPresenceOffASubscribeIsForwarded) {
  NotifierFixture f;
  f.config->presence_enable = false;
  f.receive(f.caller, NotifierFixture::subscribe());

  EXPECT_EQ(f.requests_with(f.callee_connection, "SUBSCRIBE").size(), 1u);
  EXPECT_TRUE(f.subscriptions().empty());
}

// RFC 6665 4.2.1.2: a refresh is answered with the new expiry and a NOTIFY with the current state. RFC 3261
// 12.2.1.1: each NOTIFY in the dialog has a higher CSeq. RFC 4235 4.1: and a higher version.
TEST(NotifierTest, ARefreshExtendsTheSubscriptionAndNotifiesAgain) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  auto ok = f.ok();
  auto first = f.last_notify();
  ASSERT_NE(first, nullptr);
  f.receive(f.caller, NotifierFixture::answer(first));

  f.receive(f.caller, NotifierFixture::refresh(ok, "900", 2, "z9hG4bK-sub2"));

  auto oks = f.responses_with(f.caller_connection, 200);
  ASSERT_EQ(oks.size(), 2u);
  EXPECT_EQ(NotifierFixture::value_of(oks.back(), "Expires"), "900");

  auto notifies = f.notifies();
  ASSERT_EQ(notifies.size(), 2u);
  EXPECT_GT(NotifierFixture::cseq_of(notifies[1]), NotifierFixture::cseq_of(notifies[0]));
  EXPECT_NE(notifies[1]->body.find("version=\"1\""), std::string::npos);
  EXPECT_EQ(f.subscriptions().size(), 1u);
}

// RFC 3261 12.2.2: a request in a dialog that does not exist is 481.
TEST(NotifierTest, ARefreshForNoSuchSubscriptionIs481) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  auto ok = f.ok();
  ASSERT_NE(ok, nullptr);

  f.receive(f.caller, NotifierFixture::refresh(ok, "900", 2, "z9hG4bK-sub2", "not-the-tag"));
  EXPECT_NE(f.response_with(f.caller_connection, 481), nullptr);
}

// RFC 6665 4.1.2.3: Expires 0 ends the subscription, with a final NOTIFY saying so.
TEST(NotifierTest, UnsubscribingEndsItWithAFinalNotify) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  auto ok = f.ok();
  f.receive(f.caller, NotifierFixture::answer(f.last_notify()));

  f.receive(f.caller, NotifierFixture::refresh(ok, "0", 2, "z9hG4bK-sub2"));

  EXPECT_EQ(f.responses_with(f.caller_connection, 200).size(), 2u);
  auto last = f.last_notify();
  ASSERT_EQ(f.notifies().size(), 2u);
  EXPECT_EQ(NotifierFixture::value_of(last, "Subscription-State").rfind("terminated", 0), 0u);
  EXPECT_TRUE(f.subscriptions().empty());
}

// RFC 6665 4.2.2: a subscription that is not refreshed ends at its expiry, with a NOTIFY giving the reason.
TEST(NotifierTest, AnUnrefreshedSubscriptionExpiresWithAFinalNotify) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  f.receive(f.caller, NotifierFixture::answer(f.last_notify()));

  f.advance(std::chrono::seconds(599));
  EXPECT_EQ(f.notifies().size(), 1u) << "not yet";

  f.advance(std::chrono::seconds(2));
  ASSERT_EQ(f.notifies().size(), 2u);
  EXPECT_EQ(NotifierFixture::value_of(f.last_notify(), "Subscription-State"), "terminated;reason=timeout");
  EXPECT_TRUE(f.subscriptions().empty());
}

// A refresh moves the expiry: the old one passing ends nothing.
TEST(NotifierTest, ARefreshedSubscriptionOutlivesItsFirstExpiry) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  auto ok = f.ok();
  f.receive(f.caller, NotifierFixture::answer(f.last_notify()));

  f.advance(std::chrono::seconds(300));
  f.receive(f.caller, NotifierFixture::refresh(ok, "600", 2, "z9hG4bK-sub2"));
  f.receive(f.caller, NotifierFixture::answer(f.last_notify()));

  f.advance(std::chrono::seconds(400));
  EXPECT_EQ(f.subscriptions().size(), 1u);
  EXPECT_EQ(NotifierFixture::value_of(f.last_notify(), "Subscription-State").rfind("active", 0), 0u);
}

// RFC 6665 4.2.2: a NOTIFY answered 481 removes the subscription.
TEST(NotifierTest, ANotifyAnswered481EndsTheSubscription) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe());
  f.receive(f.caller, NotifierFixture::answer(f.last_notify(), 481, "Call/Transaction Does Not Exist"));

  EXPECT_TRUE(f.subscriptions().empty());
}

// RFC 6665 4.2.1.1: a SUBSCRIBE with Expires 0 is a fetch: the state once, and no subscription.
TEST(NotifierTest, ASubscribeWithExpiresZeroFetchesTheStateOnce) {
  NotifierFixture f;
  f.receive(f.caller, NotifierFixture::subscribe("0"));

  EXPECT_NE(f.ok(), nullptr);
  auto notify = f.last_notify();
  ASSERT_NE(notify, nullptr);
  EXPECT_EQ(NotifierFixture::value_of(notify, "Subscription-State"), "terminated;reason=timeout");
  EXPECT_NE(notify->body.find("<dialog-info"), std::string::npos);
  EXPECT_TRUE(f.subscriptions().empty());
}

// RFC 6665 7.2.2: the node says which packages it is the notifier for when asked what it supports.
TEST(NotifierTest, OptionsListsThePackagesTheNodeNotifiesFor) {
  NotifierFixture f;
  std::string raw = "OPTIONS sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=z9hG4bK-options\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "From: <sip:alice@example.com>;tag=options\r\n";
  raw += "To: <sip:example.com>\r\n";
  raw += "Call-ID: options-1\r\n";
  raw += "CSeq: 1 OPTIONS\r\n";
  raw += "\r\n";
  f.receive(f.caller, raw);

  auto ok = f.ok();
  ASSERT_NE(ok, nullptr);
  EXPECT_NE(NotifierFixture::value_of(ok, "Allow-Events").find("dialog"), std::string::npos);
}
