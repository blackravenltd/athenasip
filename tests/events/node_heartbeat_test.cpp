//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/core_fixture_helper.h"
#include "../helpers/recording_event_system_helper.h"
#include "../mocks/logger_mock.h"
#include "events/topics.h"
#include "media/builtin_media_engine.h"
#include "types/url.h"

using namespace athenasip;

namespace {

struct HeartbeatFixture : CoreFixture {
  std::shared_ptr<RecordingEventSystem> bus_record = std::make_shared<RecordingEventSystem>();

  explicit HeartbeatFixture(std::uint32_t interval_seconds) {
    config->sip_node_id = "test-node";
    config->events_status_interval = interval_seconds;
    config->udp_enable = true;
    config->udp_address = "0.0.0.0";
    config->udp_port = 5060;
    config->sip_public_address = "203.0.113.5";

    core->events = bus_record;
    core->version_set("9.9.9");

    // The order a node starts in, and the order main starts it in: the will goes in
    // while the bus is still closed, because that is the only time a broker takes one,
    // and the heartbeat starts once the bus is up.
    bus_record->will_set(events::topics::node_status("test-node"), Core::node_status_json("down", "test-node", "9.9.9", "memory 0.0.1", 0, interval_seconds));
    bus_record->connect(core->strand(), [](plugins::Status) {});

    on_strand([this]() { core->node_status_start(); });
    settle();
  }

  void advance(std::chrono::seconds by) {
    on_strand([this, by]() { timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
    settle();
  }

  // Every status message the bus has seen, newest last.
  std::vector<std::string> statuses() {
    std::vector<std::string> found;
    for (const auto& published : bus_record->published()) {
      if (published.first == events::topics::node_status("test-node")) found.push_back(published.second);
    }
    return found;
  }

  boost::json::object latest_status() {
    const auto all = statuses();
    if (all.empty()) return {};
    return boost::json::parse(all.back()).as_object();
  }
};

}  // namespace

// A monitor asks "is it alive", and the only honest answer is one the node keeps
// giving. A message published once at startup says a node started, which is a
// different question and one nobody is asking an hour later.
TEST(NodeHeartbeatTest, TheNodeSaysItIsAliveOnAnInterval) {
  HeartbeatFixture f(30);

  const auto at_start = f.statuses().size();
  ASSERT_GE(at_start, 1u) << "a node should say what it is as soon as it can";

  f.advance(std::chrono::seconds(30));
  EXPECT_EQ(f.statuses().size(), at_start + 1);

  f.advance(std::chrono::seconds(30));
  EXPECT_EQ(f.statuses().size(), at_start + 2);

  f.advance(std::chrono::seconds(30));
  EXPECT_EQ(f.statuses().size(), at_start + 3);
}

// And it says the same things the HTTP health endpoint says, because a monitor that
// reads one and a monitor that reads the other should not disagree about the node.
TEST(NodeHeartbeatTest, TheHeartbeatSaysWhatTheNodeIs) {
  HeartbeatFixture f(30);

  const auto status = f.latest_status();

  EXPECT_EQ(status.at("status").as_string(), "ok");
  EXPECT_EQ(status.at("node").as_string(), "test-node");
  EXPECT_EQ(status.at("version").as_string(), "9.9.9");
  EXPECT_TRUE(status.contains("datastore"));

  // What a monitor needs beyond identity: when this was said, and how long the node has
  // been up. A timestamp is what makes a retained message readable as "still alive"
  // rather than "alive at some point".
  EXPECT_TRUE(status.contains("at"));
  EXPECT_TRUE(status.contains("uptime"));
}

// Retained, or a monitor that connects after the node did learns nothing at all until
// the next interval - and with a long interval that is a long time to look dead.
TEST(NodeHeartbeatTest, TheHeartbeatIsPublishedAsState) {
  HeartbeatFixture f(30);

  const auto states = f.bus_record->states();

  ASSERT_FALSE(states.empty()) << "the heartbeat has to be retained, not fired and forgotten";
  EXPECT_EQ(states.back().first, events::topics::node_status("test-node"));
}

// Zero turns it off, for somebody whose broker is not theirs to fill with traffic.
TEST(NodeHeartbeatTest, ZeroTurnsItOff) {
  HeartbeatFixture f(0);

  const auto at_start = f.statuses().size();
  f.advance(std::chrono::seconds(300));

  EXPECT_EQ(f.statuses().size(), at_start);
}

// A node that dies does not get to publish anything, so the broker has to say it for
// us.
TEST(NodeHeartbeatTest, TheBrokerIsToldWhatToSayIfTheNodeVanishes) {
  HeartbeatFixture f(30);

  const auto will = f.bus_record->will();

  ASSERT_TRUE(will.has_value());
  EXPECT_EQ(will->first, events::topics::node_status("test-node"));

  const auto body = boost::json::parse(will->second).as_object();
  EXPECT_EQ(body.at("status").as_string(), "down");
  EXPECT_EQ(body.at("node").as_string(), "test-node");
}

// And it is only a will if the broker was told before the session opened. Setting one
// afterwards is silently too late - the node runs, the heartbeat works, and the thing
// that was supposed to report its death never fires. That is exactly what happened the
// first time this was deployed.
TEST(NodeHeartbeatTest, AWillSetAfterConnectingIsRefusedRatherThanSilentlyLost) {
  HeartbeatFixture f(30);

  const auto before = f.bus_record->will();

  f.bus_record->will_set("nodes/somewhere/else", "{}");

  EXPECT_EQ(f.bus_record->will()->first, before->first) << "a bus that is already connected cannot take a new will";
}

// Where the node listens is in what it says, so that every node hearing it can tell a
// client where else to go (the 2026-09-21 decision). The advertised address, as the
// node list gives it: a client cannot dial a wildcard.
TEST(NodeHeartbeatTest, TheStatusSaysWhereTheNodeListens) {
  HeartbeatFixture f(30);

  const auto status = f.latest_status();
  ASSERT_TRUE(status.contains("transports")) << boost::json::serialize(status);

  const auto& transports = status.at("transports").as_array();
  ASSERT_FALSE(transports.empty());
  for (const auto& transport : transports) {
    EXPECT_FALSE(transport.at("uri").as_string().empty());
    EXPECT_NE(transport.at("address").as_string(), "0.0.0.0");
  }
}

// How often the node promises to repeat itself, so a monitor can derive its own staleness
// threshold rather than hardcoding one that happens to match this node's configuration.
TEST(NodeHeartbeatTest, TheStatusSaysHowOftenItWillBeRepeated) {
  HeartbeatFixture f(45);

  const auto status = f.latest_status();
  ASSERT_TRUE(status.contains("status_interval")) << boost::json::serialize(status);
  EXPECT_EQ(status.at("status_interval").as_int64(), 45);
}

// A node in a cluster says where its peers reach it, and one that is not says nothing.
TEST(NodeHeartbeatTest, TheStatusSaysWhereAPeerDialsWhenTheNodeIsInACluster) {
  HeartbeatFixture alone(30);
  EXPECT_FALSE(alone.latest_status().contains("cluster"));

  HeartbeatFixture f(30);
  f.on_strand([&f]() {
    f.config->cluster_enable = true;
    f.config->cluster_advertise = "10.0.0.1";
    f.config->cluster_port = 5062;
  });
  f.advance(std::chrono::seconds(30));

  const auto status = f.latest_status();
  ASSERT_TRUE(status.contains("cluster")) << boost::json::serialize(status);
  EXPECT_EQ(status.at("cluster").at("address").as_string(), "10.0.0.1");
  EXPECT_EQ(status.at("cluster").at("port").as_int64(), 5062);
}

// What the node can do with media, so a monitor, the console and a peer deciding where a
// call should be anchored can tell a node with rtpengine from one with the builtin relay
// without asking it: the engine, its capabilities and the profiles it can produce. A node
// with no engine says null rather than nothing, so "none" is not mistaken for "not said".
TEST(NodeHeartbeatTest, TheStatusSaysWhatTheNodeCanDoWithMedia) {
  HeartbeatFixture f(30);

  const auto without = f.latest_status();
  ASSERT_TRUE(without.contains("media")) << boost::json::serialize(without);
  EXPECT_TRUE(without.at("media").is_null());

  auto logger = std::make_shared<MockLogger>();
  auto engine = std::make_shared<media::BuiltinMediaEngine>(
      logger, std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=127.0.0.1&port_min=26000&port_max=26010"));
  f.on_strand([&f, engine]() { f.core->media_register(engine); });
  f.advance(std::chrono::seconds(30));

  const auto status = f.latest_status();
  ASSERT_TRUE(status.at("media").is_object()) << boost::json::serialize(status);

  const auto& media = status.at("media").as_object();
  EXPECT_NE(std::string(media.at("engine").as_string()).find("builtin"), std::string::npos);

  const auto& capabilities = media.at("capabilities").as_array();
  ASSERT_EQ(capabilities.size(), 1u);
  EXPECT_EQ(capabilities[0].as_string(), "bridge");

  const auto& produces = media.at("produces").as_array();
  ASSERT_EQ(produces.size(), 1u);
  EXPECT_EQ(produces[0].as_string(), "rtp");
}
