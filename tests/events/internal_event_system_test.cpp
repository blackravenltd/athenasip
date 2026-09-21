//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../helpers/sync_event_system_helper.h"
#include "../helpers/wait_for_condition_helper.h"
#include "../mocks/logger_mock.h"
#include "events/local_event_system.h"
#include "global_io_context.h"
#include "loggers/logger.h"

using namespace athenasip;

namespace {

// Subscribing is a round trip by contract, so a test that wants a subscription before
// it publishes waits for one. SyncEventSystem is the only thing here allowed to wait.
SyncEventSystem connected_bus() {
  SyncEventSystem bus(std::make_shared<events::LocalEventSystem>(std::make_shared<MockLogger>()));
  EXPECT_TRUE(bus.connect());
  return bus;
}

}  // namespace

// Publishing an event triggers a subscribed callback.
TEST(EventSystemTest, PublishTriggersSubscription) {
  auto bus = connected_bus();

  std::atomic<bool> callback_called(false);

  auto subscription = bus.subscribe("TestEvent", [&callback_called](std::string, std::string) { callback_called.store(true); });
  ASSERT_NE(subscription, nullptr);

  EXPECT_TRUE(bus.publish("TestEvent", "Hello"));

  EXPECT_TRUE(waitForCondition(callback_called, std::chrono::milliseconds(100)));
}

// The fire-and-forget publish delivers the same event, and answers nothing: a caller on
// the call path announces what happened without waiting for the bus.
TEST(EventSystemTest, FireAndForgetPublishTriggersSubscription) {
  auto bus = connected_bus();

  std::atomic<bool> callback_called(false);

  ASSERT_NE(bus.subscribe("TestEvent", [&callback_called](std::string, std::string) { callback_called.store(true); }), nullptr);

  bus.driver()->publish("TestEvent", "Hello");

  EXPECT_TRUE(waitForCondition(callback_called, std::chrono::milliseconds(100)));
}

// Unsubscribing prevents the callback from being invoked.
TEST(EventSystemTest, UnsubscribePreventsCallback) {
  auto bus = connected_bus();

  std::atomic<bool> callback_called(false);

  auto subscription = bus.subscribe("TestEvent", [&callback_called](std::string, std::string) { callback_called.store(true); });
  ASSERT_NE(subscription, nullptr);

  EXPECT_TRUE(bus.unsubscribe(subscription));
  EXPECT_TRUE(bus.publish("TestEvent", "Hello"));

  EXPECT_FALSE(waitForCondition(callback_called, std::chrono::milliseconds(100)));
}

// unsubscribe_all stops every callback.
TEST(EventSystemTest, UnsubscribeAllPreventsCallbacks) {
  auto bus = connected_bus();

  std::atomic<bool> callback1_called(false);
  std::atomic<bool> callback2_called(false);

  ASSERT_NE(bus.subscribe("TestEvent", [&callback1_called](std::string, std::string) { callback1_called.store(true); }), nullptr);
  ASSERT_NE(bus.subscribe("TestEvent", [&callback2_called](std::string, std::string) { callback2_called.store(true); }), nullptr);

  EXPECT_TRUE(bus.unsubscribe_all());
  EXPECT_TRUE(bus.publish("TestEvent", "Hello"));

  EXPECT_FALSE(waitForCondition(callback1_called, std::chrono::milliseconds(100)));
  EXPECT_FALSE(waitForCondition(callback2_called, std::chrono::milliseconds(100)));
}

// Several subscriptions to the same event are all triggered.
TEST(EventSystemTest, MultipleSubscriptionsTriggered) {
  auto bus = connected_bus();

  std::atomic<int> call_count(0);

  ASSERT_NE(bus.subscribe("TestEvent", [&call_count](std::string, std::string) { call_count.fetch_add(1); }), nullptr);
  ASSERT_NE(bus.subscribe("TestEvent", [&call_count](std::string, std::string) { call_count.fetch_add(1); }), nullptr);

  EXPECT_TRUE(bus.publish("TestEvent", "Hello"));

  auto start = std::chrono::steady_clock::now();
  while (call_count.load() < 2 && (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    detail::get_global_io_context().poll();
  }

  EXPECT_EQ(call_count.load(), 2);
}

// A publish to a closed bus is refused rather than silently dropped.
TEST(EventSystemTest, PublishWhileClosedFails) {
  auto bus = connected_bus();
  bus.close();

  EXPECT_FALSE(bus.is_connected());
  EXPECT_FALSE(bus.publish("TestEvent", "Hello"));
}
