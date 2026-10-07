//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "events/mqtt_event_system.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/sync_event_system_helper.h"
#include "../mocks/logger_mock.h"
#include "events/topics.h"
#include "types/url.h"

using namespace athenasip;
using athenasip::events::MQTTEventSystem;

// These run against a real broker and skip without ATHENA_TEST_MQTT_URL:
//
//   mosquitto -p 1883 -d
//   ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests --gtest_filter='MqttEventSystemTest.*'
namespace {

std::string broker_url() {
  const char* url = std::getenv("ATHENA_TEST_MQTT_URL");
  return url ? std::string(url) : std::string();
}

std::shared_ptr<SyncEventSystem> connect_to(const std::string& suffix = "") {
  const auto url = broker_url();
  if (url.empty()) return nullptr;

  auto logger = std::make_shared<MockLogger>();
  auto events = std::make_shared<MQTTEventSystem>(logger, std::make_shared<types::URL>(url + suffix));

  auto bus = std::make_shared<SyncEventSystem>(events);
  if (!bus->connect()) return nullptr;

  return bus;
}

// A topic unique to this run, so two runs against one broker do not see each other's messages.
std::string unique_topic(const std::string& leaf) {
  static std::atomic<int> counter{0};
  return "athenatest/" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) + "/" + leaf;
}

// What a consumer saw, under a lock: the callback arrives on the bus's own thread.
struct Received {
  std::mutex mutex;
  std::vector<std::pair<std::string, std::string>> messages;

  void add(const std::string& topic, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex);
    messages.emplace_back(topic, payload);
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock(mutex);
    return messages.size();
  }

  std::vector<std::pair<std::string, std::string>> all() {
    std::lock_guard<std::mutex> lock(mutex);
    return messages;
  }
};

// Waits for the count rather than sleeping a fixed time: delivery through a broker is eventual.
bool eventually(Received& received, std::size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (std::chrono::steady_clock::now() < deadline) {
    if (received.size() >= count) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return received.size() >= count;
}

// A fixed wait, for asserting that nothing arrived.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(400)); }

#define SKIP_WITHOUT_BROKER(bus)                                                               \
  if ((bus) == nullptr) {                                                                      \
    GTEST_SKIP() << "set ATHENA_TEST_MQTT_URL to a broker to run the MQTT event system tests"; \
  }

}  // namespace

// A wrong broker address fails connect(): boost.mqtt5 reconnects for ever on its own, so only a round trip tells.
TEST(MqttEventSystemTest, ConnectFailsWhenNoBrokerIsThere) {
  if (broker_url().empty()) GTEST_SKIP() << "set ATHENA_TEST_MQTT_URL to a broker to run the MQTT event system tests";

  auto logger = std::make_shared<MockLogger>();

  // Port 1 on loopback: privileged, unbound, and refused immediately.
  auto events = std::make_shared<MQTTEventSystem>(logger, std::make_shared<types::URL>("mqtt://127.0.0.1:1/?connect_timeout_ms=800"));

  SyncEventSystem bus(events);

  EXPECT_FALSE(bus.connect());
  EXPECT_FALSE(bus.is_connected());

  bus.close();
}

TEST(MqttEventSystemTest, ConnectsAndReportsConnected) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  EXPECT_TRUE(bus->is_connected());
  EXPECT_EQ(bus->name(), "mqtt");

  bus->close();
  EXPECT_FALSE(bus->is_connected());
}

TEST(MqttEventSystemTest, APublishedMessageReachesAConsumerOnTheSameTopic) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto topic = unique_topic("status");
  Received received;

  auto subscription = bus->subscribe(topic, [&received](const std::string& name, const std::string& payload) { received.add(name, payload); });
  ASSERT_NE(subscription, nullptr) << bus->last_error();

  EXPECT_TRUE(bus->publish(topic, "{\"started\":\"now\"}"));

  ASSERT_TRUE(eventually(received, 1));

  const auto messages = received.all();
  EXPECT_EQ(messages[0].first, topic);
  EXPECT_EQ(messages[0].second, "{\"started\":\"now\"}");

  bus->close();
}

// A node publishes and another process hears it, which cluster discovery depends on.
TEST(MqttEventSystemTest, OneClientHearsWhatAnotherPublishes) {
  auto publisher = connect_to();
  auto consumer = connect_to();

  SKIP_WITHOUT_BROKER(publisher);
  SKIP_WITHOUT_BROKER(consumer);

  const auto topic = unique_topic("nodes/sip-0002/status");
  Received received;

  auto subscription = consumer->subscribe(topic, [&received](const std::string& name, const std::string& payload) { received.add(name, payload); });
  ASSERT_NE(subscription, nullptr) << consumer->last_error();

  EXPECT_TRUE(publisher->publish(topic, "{\"started\":\"2026-09-22T00:00:00Z\"}"));

  ASSERT_TRUE(eventually(received, 1));
  EXPECT_EQ(received.all()[0].second, "{\"started\":\"2026-09-22T00:00:00Z\"}");

  publisher->close();
  consumer->close();
}

// MQTT's wildcard rules, as docs/events.md promises: + is exactly one level and # is the rest.
TEST(MqttEventSystemTest, PlusMatchesOneLevelAndHashMatchesTheRest) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto root = unique_topic("nodes");
  Received single;
  Received rest;

  ASSERT_NE(bus->subscribe(root + "/+/status", [&single](const std::string& n, const std::string& p) { single.add(n, p); }), nullptr);
  ASSERT_NE(bus->subscribe(root + "/#", [&rest](const std::string& n, const std::string& p) { rest.add(n, p); }), nullptr);

  EXPECT_TRUE(bus->publish(root + "/sip-0001/status", "up"));
  EXPECT_TRUE(bus->publish(root + "/sip-0001/channels/udp/1.2.3.4:5060", "registered"));

  ASSERT_TRUE(eventually(rest, 2));
  ASSERT_TRUE(eventually(single, 1));

  settle();

  // One level deeper than the filter allows, so + must not have matched it.
  EXPECT_EQ(single.size(), 1u);
  EXPECT_EQ(single.all()[0].first, root + "/sip-0001/status");
  EXPECT_EQ(rest.size(), 2u);

  bus->close();
}

TEST(MqttEventSystemTest, AnUnsubscribedCallbackHearsNothingMore) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto topic = unique_topic("goes-away");
  Received received;

  auto subscription = bus->subscribe(topic, [&received](const std::string& n, const std::string& p) { received.add(n, p); });
  ASSERT_NE(subscription, nullptr);

  EXPECT_TRUE(bus->publish(topic, "first"));
  ASSERT_TRUE(eventually(received, 1));

  EXPECT_TRUE(bus->unsubscribe(subscription));

  EXPECT_TRUE(bus->publish(topic, "second"));
  settle();

  EXPECT_EQ(received.size(), 1u);

  bus->close();
}

TEST(MqttEventSystemTest, UnsubscribeAllLeavesNothingListening) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto first = unique_topic("one");
  const auto second = unique_topic("two");
  Received received;

  ASSERT_NE(bus->subscribe(first, [&received](const std::string& n, const std::string& p) { received.add(n, p); }), nullptr);
  ASSERT_NE(bus->subscribe(second, [&received](const std::string& n, const std::string& p) { received.add(n, p); }), nullptr);

  EXPECT_TRUE(bus->unsubscribe_all());

  EXPECT_TRUE(bus->publish(first, "x"));
  EXPECT_TRUE(bus->publish(second, "y"));
  settle();

  EXPECT_EQ(received.size(), 0u);

  bus->close();
}

// The prefix keeps two clusters on one broker apart. It is added on the wire and stripped before a consumer
// sees the topic.
TEST(MqttEventSystemTest, ThePrefixIsAppliedOnTheWireAndStrippedOnTheWayBack) {
  // No trailing separator: the driver puts the '/' between the prefix and the topic.
  const auto prefix = "athenatest-prefix-" + std::to_string(::getpid());

  auto prefixed = connect_to("/?prefix=" + prefix);
  auto plain = connect_to("/?prefix=");

  SKIP_WITHOUT_BROKER(prefixed);
  SKIP_WITHOUT_BROKER(plain);

  const auto topic = unique_topic("prefixed");

  Received inside;
  Received outside;

  ASSERT_NE(prefixed->subscribe(topic, [&inside](const std::string& n, const std::string& p) { inside.add(n, p); }), nullptr);
  ASSERT_NE(plain->subscribe(prefix + "/" + topic, [&outside](const std::string& n, const std::string& p) { outside.add(n, p); }), nullptr);

  EXPECT_TRUE(prefixed->publish(topic, "hello"));

  ASSERT_TRUE(eventually(inside, 1));
  ASSERT_TRUE(eventually(outside, 1));

  // The consumer inside the prefix sees the topic it asked for, not the wire topic.
  EXPECT_EQ(inside.all()[0].first, topic);

  // On the wire the prefix is there.
  EXPECT_EQ(outside.all()[0].first, prefix + "/" + topic);

  prefixed->close();
  plain->close();
}

// A prefixed client does not hear an unprefixed publish.
TEST(MqttEventSystemTest, APrefixedClientDoesNotHearAnUnprefixedPublish) {
  const auto prefix = "athenatest-apart-" + std::to_string(::getpid());

  auto prefixed = connect_to("/?prefix=" + prefix);
  auto plain = connect_to("/?prefix=");

  SKIP_WITHOUT_BROKER(prefixed);
  SKIP_WITHOUT_BROKER(plain);

  const auto topic = unique_topic("apart");
  Received inside;

  ASSERT_NE(prefixed->subscribe(topic, [&inside](const std::string& n, const std::string& p) { inside.add(n, p); }), nullptr);

  EXPECT_TRUE(plain->publish(topic, "not for you"));
  settle();

  EXPECT_EQ(inside.size(), 0u);

  prefixed->close();
  plain->close();
}

// The fire-and-forget publish answers nothing, so what proves it worked is what the consumer saw.
TEST(MqttEventSystemTest, TheFireAndForgetPublishStillDelivers) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto topic = unique_topic("fire-and-forget");
  Received received;

  ASSERT_NE(bus->subscribe(topic, [&received](const std::string& n, const std::string& p) { received.add(n, p); }), nullptr);

  bus->driver()->publish(topic, "sent and not waited for");

  ASSERT_TRUE(eventually(received, 1));
  EXPECT_EQ(received.all()[0].second, "sent and not waited for");

  bus->close();
}

// The node's own topics through the real driver. No topic begins with a slash: a leading slash is an empty
// first level in MQTT, which a nodes/# filter does not match.
TEST(MqttEventSystemTest, TheNodeTopicSchemeSurvivesTheRoundTrip) {
  auto bus = connect_to();
  SKIP_WITHOUT_BROKER(bus);

  const auto scoped = "athenatest-" + std::to_string(::getpid());
  const auto topic = scoped + "/" + events::topics::node_status("sip-0001");

  ASSERT_NE(topic.front(), '/');

  Received received;
  ASSERT_NE(bus->subscribe(scoped + "/nodes/#", [&received](const std::string& n, const std::string& p) { received.add(n, p); }), nullptr);

  EXPECT_TRUE(bus->publish(topic, "{\"started\":\"now\"}"));

  ASSERT_TRUE(eventually(received, 1));
  EXPECT_EQ(received.all()[0].first, topic);

  bus->close();
}

// One mqtt_client is reused across connect/close cycles, so a completion from a finished run may arrive during
// the next. Each run carries a generation so a late completion cannot reset the new run's work guard or clear
// its subscriptions. This exercises the cycle; it cannot force the interleaving.
TEST(MqttEventSystemTest, AClosedRunDoesNotTakeTheNextOneDownWithIt) {
  const auto url = broker_url();
  if (url.empty()) GTEST_SKIP() << "ATHENA_TEST_MQTT_URL is not set";

  auto logger = std::make_shared<MockLogger>();
  auto events = std::make_shared<MQTTEventSystem>(logger, std::make_shared<types::URL>(url));
  auto bus = std::make_shared<SyncEventSystem>(events);

  const auto topic = unique_topic("generation");

  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_TRUE(bus->connect()) << "cycle " << cycle;
    EXPECT_TRUE(bus->is_connected()) << "cycle " << cycle;

    // A publish needs this run's work guard, which a stale completion would have released.
    EXPECT_TRUE(bus->publish(topic, "still here")) << "cycle " << cycle;

    bus->close();
    EXPECT_FALSE(bus->is_connected()) << "cycle " << cycle;
  }
}
