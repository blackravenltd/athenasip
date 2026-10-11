//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "call.h"
#include "helpers/proxy_fixture_helper.h"
#include "types/presence_record.h"

using namespace athenasip;

namespace {

// Carol watches Alice's presence and Bob's line. What changes here changes in the datastore behind the notifier's
// back, as it does when the bus drops a signal or a record lapses: only the reconcile can find it
// (docs/design/presence.md, The cluster).
struct ReconcileFixture : ProxyFixture {
  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;
  int subscriptions = 0;

  ReconcileFixture() {
    bind_bob();
    seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    carol = make_channel("192.0.2.30", &carol_connection);
    on_strand([this]() { carol->authenticated_as("sip:carol@example.com"); });
  }

  void watch(const std::string& presentity, const std::string& package) {
    ++subscriptions;
    std::string raw = "SUBSCRIBE " + presentity + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "To: <" + presentity + ">\r\n";
    raw += "Call-ID: watch-" + std::to_string(subscriptions) + "\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: " + package + "\r\n";
    raw += "Expires: 3600\r\n";
    raw += "\r\n";
    receive(carol, raw);
    answer_notifies();
  }

  void advance(std::chrono::seconds by) {
    for (auto step = std::chrono::seconds(0); step < by; step += std::chrono::seconds(5)) {
      on_strand([this]() { timers->advance(std::chrono::seconds(5)); });
      answer_notifies();
    }
  }

  void answer_notifies() {
    settle();
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
    settle();
  }

  std::vector<std::shared_ptr<SIPMessage>> notifies(int subscription) {
    std::vector<std::shared_ptr<SIPMessage>> found;
    for (const auto& notify : requests_with(carol_connection, "NOTIFY")) {
      if (notify->header->headers_map["Call-ID"][0]->to_string() == "watch-" + std::to_string(subscription)) found.push_back(notify);
    }
    return found;
  }

  std::string latest(int subscription) {
    auto all = notifies(subscription);
    return all.empty() ? "" : all.back()->body;
  }

  // A publication written straight to the datastore: no PUBLISH, so nothing on the bus.
  void published_unseen(const std::string& aor, const std::string& tuple) {
    types::PresenceRecord record;
    record.kind = types::PresenceRecord::Kind::Publication;
    record.id = "unseen-" + tuple;
    record.aor = aor;
    record.content_type = "application/pidf+xml";
    record.body = "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" entity=\"" + aor + "\"><tuple id=\"" + tuple +
                  "\"><status><basic>open</basic></status></tuple></presence>";
    ASSERT_TRUE(store->presence_put(record, 600));
  }

 private:
  std::set<std::string> _answered;
};

}  // namespace

// A publication the bus never told anyone of is seen within one reconcile.
TEST(ReconcileTest, AChangeTheBusDidNotCarryIsFoundByTheReconcile) {
  ReconcileFixture f;
  f.watch("sip:alice@example.com", "presence");
  ASSERT_EQ(f.notifies(1).size(), 1u);

  f.published_unseen("sip:alice@example.com", "softphone");
  f.advance(std::chrono::seconds(10));
  EXPECT_EQ(f.notifies(1).size(), 1u) << "not before the reconcile";

  f.advance(std::chrono::seconds(25));
  ASSERT_EQ(f.notifies(1).size(), 2u);
  EXPECT_NE(f.latest(1).find("softphone"), std::string::npos) << f.latest(1);
}

// A call that ended without a signal frees the line at the next reconcile.
TEST(ReconcileTest, ACallClosedUnseenIsFoundByTheReconcile) {
  ReconcileFixture f;

  auto call = std::make_shared<Call>();
  call->id = "unseen-call";
  call->node = "test-node";
  call->state = Call::State::Connected;
  call->created_at = std::time(nullptr);
  Call::Participant caller;
  caller.originator = true;
  caller.identity = std::make_shared<types::SIPIdentity>("sip:alice@example.com");
  caller.subscriber = "sip:alice@example.com";
  Call::Participant callee;
  callee.identity = std::make_shared<types::SIPIdentity>("sip:bob@example.com");
  callee.subscriber = "sip:bob@example.com";
  call->participants = {caller, callee};
  ASSERT_TRUE(f.store->call_create(call));

  f.watch("sip:bob@example.com", "dialog");
  ASSERT_NE(f.latest(1).find("confirmed"), std::string::npos) << f.latest(1);

  call->state = Call::State::Closed;
  ASSERT_TRUE(f.store->call_update(call));
  f.advance(std::chrono::seconds(35));

  ASSERT_EQ(f.notifies(1).size(), 2u);
  EXPECT_EQ(f.latest(1).find("<dialog "), std::string::npos) << f.latest(1);
}

// Nothing changed, nothing is sent: a dialog-info's version rising is not a change.
TEST(ReconcileTest, NothingChangedNothingIsSent) {
  ReconcileFixture f;
  f.watch("sip:alice@example.com", "presence");
  f.watch("sip:bob@example.com", "dialog");

  f.advance(std::chrono::seconds(95));
  EXPECT_EQ(f.notifies(1).size(), 1u);
  EXPECT_EQ(f.notifies(2).size(), 1u);
}

// presence.reconcile 0 never rereads.
TEST(ReconcileTest, ItCanBeTurnedOff) {
  ReconcileFixture f;
  f.config->presence_reconcile = 0;
  f.watch("sip:alice@example.com", "presence");

  f.published_unseen("sip:alice@example.com", "softphone");
  f.advance(std::chrono::seconds(95));
  EXPECT_EQ(f.notifies(1).size(), 1u);
}
