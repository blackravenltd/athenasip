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

#include "helpers/proxy_fixture_helper.h"
#include "types/presence_record.h"

using namespace athenasip;

namespace {

using Kind = types::PresenceRecord::Kind;

// Carol watches Bob's presence from her desk phone, which is registered here as sip:carol@192.0.2.30:5060 over a
// connection of its own (docs/design/presence.md, Requests leaving it, and The cluster).
struct TakeoverFixture : ProxyFixture {
  std::shared_ptr<types::Subscriber> carol_subscriber;
  std::shared_ptr<MockConnection> watch_connection;
  std::shared_ptr<Channel> watch_channel;
  std::shared_ptr<MockConnection> binding_connection;
  std::shared_ptr<Channel> binding_channel;

  TakeoverFixture() {
    config->events_status_interval = 30;
    bind_bob();
    carol_subscriber = seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    watch_channel = make_channel("192.0.2.30", &watch_connection, "tcp", 50000);
    binding_channel = make_channel("192.0.2.30", &binding_connection, "tcp", 50001);
    on_strand([this]() {
      watch_channel->authenticated_as("sip:carol@example.com");
      binding_channel->authenticated_as("sip:carol@example.com");
    });
  }

  void register_carol() {
    ASSERT_TRUE(register_binding(carol_subscriber, std::make_shared<types::SIPUri>("sip:carol@192.0.2.30:5060"), binding_channel, 3600));
  }

  void watch_bob() {
    std::string raw = "SUBSCRIBE sip:bob@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-1\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-1\r\n";
    raw += "To: <sip:bob@example.com>\r\n";
    raw += "Call-ID: takeover-1\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: presence\r\n";
    raw += "Expires: 3600\r\n";
    raw += "\r\n";
    receive(watch_channel, raw);
    answer(watch_channel, watch_connection);
  }

  // The phone answers every NOTIFY on the connection it came down.
  void answer(const std::shared_ptr<Channel>& channel, const std::shared_ptr<MockConnection>& connection) {
    settle();
    for (const auto& notify : requests_with(connection, "NOTIFY")) {
      const auto cseq = notify->header->headers_map["CSeq"][0]->to_string();
      if (!_answered.insert(cseq).second) continue;

      std::string raw = "SIP/2.0 200 OK\r\n";
      for (const auto& via : notify->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
      raw += "From: " + notify->header->headers_map["From"][0]->to_string() + "\r\n";
      raw += "To: " + notify->header->headers_map["To"][0]->to_string() + "\r\n";
      raw += "Call-ID: " + notify->header->headers_map["Call-ID"][0]->to_string() + "\r\n";
      raw += "CSeq: " + cseq + "\r\n";
      raw += "\r\n";
      receive(channel, raw);
    }
    settle();
  }

  void advance(std::chrono::seconds by) {
    for (auto step = std::chrono::seconds(0); step < by; step += std::chrono::seconds(5)) {
      on_strand([this]() { timers->advance(std::chrono::seconds(5)); });
      answer(binding_channel, binding_connection);
    }
  }

  void close_the_watch_flow() {
    on_strand([this]() { watch_channel->close(); });
    settle();
  }

  // A node the bus has heard from.
  void heard(const std::string& node, const std::string& status) {
    core->nodes()->observe("nodes/" + node + "/status", "{\"status\":\"" + status + "\",\"node\":\"" + node + "\"}");
  }

  // The subscription, as though another node held it.
  void held_by(const std::string& node) {
    auto records = store->presence_list("sip:bob@example.com", Kind::Subscription);
    ASSERT_EQ(records.size(), 1u);
    auto record = records[0];
    record.node_id = node;
    ASSERT_TRUE(store->presence_put(record, 3600));
  }

  std::string holder() {
    auto records = store->presence_list("sip:bob@example.com", Kind::Subscription);
    return records.size() == 1 ? records[0].node_id : "";
  }

 private:
  std::set<std::string> _answered;
};

}  // namespace

// RFC 5626 and docs/design/presence.md: the flow the SUBSCRIBE came over has closed, so the NOTIFY goes down the
// flow of the watcher's binding whose Contact is the remote target.
TEST(TakeoverTest, ANotifyWhoseFlowHasGoneTakesTheWatchersBindingFlow) {
  TakeoverFixture f;
  f.register_carol();
  f.watch_bob();
  ASSERT_EQ(f.requests_with(f.watch_connection, "NOTIFY").size(), 1u);
  f.advance(std::chrono::seconds(10));

  f.close_the_watch_flow();
  ASSERT_TRUE(f.unregister_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), f.callee));
  f.answer(f.binding_channel, f.binding_connection);

  auto notifies = f.requests_with(f.binding_connection, "NOTIFY");
  ASSERT_EQ(notifies.size(), 1u);
  EXPECT_NE(notifies[0]->body.find("<basic>closed</basic>"), std::string::npos);
  EXPECT_EQ(f.holder(), "test-node");
}

// Nothing reaches the watcher: the subscription ends rather than lingering.
TEST(TakeoverTest, WithNoWayToTheWatcherTheSubscriptionEnds) {
  TakeoverFixture f;
  f.watch_bob();
  f.advance(std::chrono::seconds(10));

  f.close_the_watch_flow();
  ASSERT_TRUE(f.unregister_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), f.callee));
  f.settle();

  EXPECT_TRUE(f.store->presence_list("sip:bob@example.com", Kind::Subscription).empty());
}

// A node that has gone leaves its subscriptions in the datastore; the node the watcher has a flow to takes them over
// under a lease, and notifies at once.
TEST(TakeoverTest, AGoneNodesSubscriptionIsTakenOverByTheNodeWithTheWatchersFlow) {
  TakeoverFixture f;
  f.register_carol();
  f.watch_bob();
  f.held_by("node-gone");
  f.heard("node-gone", "down");

  f.advance(std::chrono::seconds(125));

  EXPECT_EQ(f.holder(), "test-node");
  auto notifies = f.requests_with(f.binding_connection, "NOTIFY");
  ASSERT_FALSE(notifies.empty());
  EXPECT_EQ(notifies.back()->header->headers_map["Subscription-State"][0]->to_string().rfind("active", 0), 0u);
  EXPECT_FALSE(f.store->lease("presence-takeover:" + f.store->presence_list("sip:bob@example.com", Kind::Subscription)[0].id, "node-other", 60))
      << "taken under a lease";
}

// A live node keeps its own.
TEST(TakeoverTest, ALiveNodesSubscriptionIsLeftAlone) {
  TakeoverFixture f;
  f.register_carol();
  f.watch_bob();
  f.held_by("node-live");

  for (int i = 0; i < 5; ++i) {
    f.heard("node-live", "ok");
    f.advance(std::chrono::seconds(30));
  }
  EXPECT_EQ(f.holder(), "node-live");
}

// Not before this node has heard the cluster for three status intervals: every other node would look gone.
TEST(TakeoverTest, NothingIsTakenOverBeforeTheClusterHasBeenHeard) {
  TakeoverFixture f;
  f.register_carol();
  f.watch_bob();
  f.held_by("node-gone");
  f.heard("node-gone", "down");

  f.advance(std::chrono::seconds(60));
  EXPECT_EQ(f.holder(), "node-gone");
}
