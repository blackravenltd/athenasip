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

// These run against a real broker. Point ATHENA_TEST_MQTT_URL at one to enable them;
// without it every case skips, so a machine with no broker still runs the suite green.
//
//   mosquitto -p 1883 -d
//   ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ctest -R MqttEventSystemTest
//
// mqtt:// is the canonical event system and the one a cluster runs on, so what is
// asserted here is what Milestone 4 depends on: that a node's events reach another
// process, that the wildcards mean what MQTT says they mean, and that a prefix keeps
// two clusters on one broker apart.
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

// A topic nobody else is using, so two runs of the suite against one broker do not see
// each other's messages.
std::string unique_topic(const std::string& leaf) {
  static std::atomic<int> counter{0};
  return "athenatest/" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) + "/" + leaf;
}

// What a subscriber saw, under a lock: the callback arrives on the bus's own thread.
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

// A broker is another process on the other side of a socket, so everything here is
// eventual. Waiting for the count rather than sleeping a fixed time keeps the tests
// quick when the broker is local and honest when it is not.
bool eventually(Received& received, std::size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (std::chrono::steady_clock::now() < deadline) {
    if (received.size() >= count) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return received.size() >= count;
}

// Nothing arrived, and it stayed that way. Used where the assertion is an absence.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(400)); }

#define SKIP_WITHOUT_BROKER(bus)                                                               \
  if ((bus) == nullptr) {                                                                      \
    GTEST_SKIP() << "set ATHENA_TEST_MQTT_URL to a broker to run the MQTT event system tests"; \
  }

}  // namespace

// A node whose broker address is wrong has to find out at startup. boost.mqtt5 queues
// and reconnects for ever on its own, so starting the client says nothing: only a
// round trip does.
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

TEST(MqttEventSystemTest, APublishedMessageReachesASubscriberOnTheSameTopic) {
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

// The whole reason the bus is MQTT rather than something in process: a node publishes
// and another node, in another process, hears it. This is how cluster discovery works
// in Milestone 4, and it is the one thing an in-process bus cannot do.
TEST(MqttEventSystemTest, OneClientHearsWhatAnotherPublishes) {
  auto publisher = connect_to();
  auto subscriber = connect_to();

  SKIP_WITHOUT_BROKER(publisher);
  SKIP_WITHOUT_BROKER(subscriber);

  const auto topic = unique_topic("nodes/sip-0002/status");
  Received received;

  auto subscription = subscriber->subscribe(topic, [&received](const std::string& name, const std::string& payload) { received.add(name, payload); });
  ASSERT_NE(subscription, nullptr) << subscriber->last_error();

  EXPECT_TRUE(publisher->publish(topic, "{\"started\":\"2026-09-22T00:00:00Z\"}"));

  ASSERT_TRUE(eventually(received, 1));
  EXPECT_EQ(received.all()[0].second, "{\"started\":\"2026-09-22T00:00:00Z\"}");

  publisher->close();
  subscriber->close();
}

// MQTT's own wildcard rules, which docs/events.md tells a consumer to rely on: + is
// exactly one level and # is the rest. A consumer subscribing to nodes/+/status to
// find the cluster is depending on this being the broker's meaning and not ours.
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

// The prefix is what keeps two clusters on one broker from hearing each other. It goes
// on in front of what this node publishes and comes off again before a subscriber sees
// the topic, so nothing above the driver has to know it is there.
TEST(MqttEventSystemTest, ThePrefixIsAppliedOnTheWireAndStrippedOnTheWayBack) {
  // Deliberately without a trailing separator: a prefix is a topic level and the
  // driver is what puts the '/' between it and the topic.
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

  // The subscriber inside the prefix sees the topic it asked for, not the wire topic.
  EXPECT_EQ(inside.all()[0].first, topic);

  // And on the wire the prefix really is there, which is what separates two clusters.
  EXPECT_EQ(outside.all()[0].first, prefix + "/" + topic);

  prefixed->close();
  plain->close();
}

// A cluster on a prefix must not hear one that is not, or the separation is decoration.
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

// The fire-and-forget publish stays on the contract because the bus is observability
// and is never on the call setup path. It answers nothing, so what proves it worked is
// what the subscriber saw.
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

// The topics the node actually publishes, through the driver that actually carries
// them. docs/events.md says no topic begins with a slash, because a leading slash is a
// distinct empty first level in MQTT and a nodes/# filter does not match it.
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

// One mqtt_client is reused across connect/close cycles, so a completion handler from a
// finished run can still be in flight while the next run is starting - and connect() sets
// _connected true before it joins the previous thread, so the handler's own guard does not
// catch it. It would then reset the new run's work guard and clear its subscriptions.
//
// Each run now carries a generation and a late completion identifies itself. This exercises
// the cycle it happens in; it does not force the interleaving, which would need a hook into
// the client thread that does not exist. Reported by the Corvus LoRa Bridge session.
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

    // Something that needs this run to still be working: a publish goes through the client
    // whose work guard a stale completion would have released.
    EXPECT_TRUE(bus->publish(topic, "still here")) << "cycle " << cycle;

    bus->close();
    EXPECT_FALSE(bus->is_connected()) << "cycle " << cycle;
  }
}
