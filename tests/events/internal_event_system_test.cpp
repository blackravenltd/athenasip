//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "events/local_event_system.h"
#include "loggers/logger.h"
#include "global_io_context.h"

#include "../mocks/logger_mock.h"
#include "../helpers/wait_for_condition_helper.h"

using namespace athenasip;

// Test that publishing an event triggers a subscribed callback.
TEST(EventSystemTest, PublishTriggersSubscription) {
    auto logger = std::make_shared<MockLogger>();
    auto event_system = std::make_shared<events::LocalEventSystem>(logger);
    event_system->connect();
    
    std::atomic<bool> callback_called(false);
    
    // Subscribe to "TestEvent" and have the callback set the flag.
    auto subscription = event_system->subscribe(
        "TestEvent",
        [&callback_called](std::string event_name, std::string message) {
            callback_called.store(true);
        },
        [](bool success) { EXPECT_TRUE(success); }
    );
    
    // Publish "TestEvent".
    event_system->publish("TestEvent", "Hello", [](bool success) { EXPECT_TRUE(success); });
    
    // Wait up to 100ms for the callback to run.
    EXPECT_TRUE(waitForCondition(callback_called, std::chrono::milliseconds(100)));
}

// Test that unsubscribing prevents the callback from being invoked.
TEST(EventSystemTest, UnsubscribePreventsCallback) {
    auto logger = std::make_shared<MockLogger>();
    auto event_system = std::make_shared<events::LocalEventSystem>(logger);
    event_system->connect();
    
    std::atomic<bool> callback_called(false);
    
    auto subscription = event_system->subscribe(
        "TestEvent",
        [&callback_called](std::string, std::string) { callback_called.store(true); },
        [](bool success) { EXPECT_TRUE(success); }
    );
    
    // Unsubscribe before publishing.
    event_system->unsubscribe(subscription, [](bool success) { EXPECT_TRUE(success); });
    
    // Publish the event.
    event_system->publish("TestEvent", "Hello", [](bool success) { EXPECT_TRUE(success); });
    
    // Wait for a short period; the callback should not be called.
    EXPECT_FALSE(waitForCondition(callback_called, std::chrono::milliseconds(100)));
}

// Test that unsubscribe_all stops all callbacks.
TEST(EventSystemTest, UnsubscribeAllPreventsCallbacks) {
    auto logger = std::make_shared<MockLogger>();
    auto event_system = std::make_shared<events::LocalEventSystem>(logger);
    event_system->connect();
    
    std::atomic<bool> callback1_called(false);
    std::atomic<bool> callback2_called(false);
    
    auto sub1 = event_system->subscribe(
        "TestEvent",
        [&callback1_called](std::string, std::string) { callback1_called.store(true); },
        [](bool success) { EXPECT_TRUE(success); }
    );
    auto sub2 = event_system->subscribe(
        "TestEvent",
        [&callback2_called](std::string, std::string) { callback2_called.store(true); },
        [](bool success) { EXPECT_TRUE(success); }
    );
    
    // Unsubscribe all.
    event_system->unsubscribe_all([](bool success) { EXPECT_TRUE(success); });
    
    // Publish the event.
    event_system->publish("TestEvent", "Hello", [](bool success) { EXPECT_TRUE(success); });
    
    EXPECT_FALSE(waitForCondition(callback1_called, std::chrono::milliseconds(100)));
    EXPECT_FALSE(waitForCondition(callback2_called, std::chrono::milliseconds(100)));
}

// Test that multiple subscriptions on the same event are all triggered.
TEST(EventSystemTest, MultipleSubscriptionsTriggered) {
    auto logger = std::make_shared<MockLogger>();
    auto event_system = std::make_shared<events::LocalEventSystem>(logger);
    event_system->connect();
    
    std::atomic<int> call_count(0);
    
    auto sub1 = event_system->subscribe(
        "TestEvent",
        [&call_count](std::string, std::string) { call_count.fetch_add(1); },
        [](bool success) { EXPECT_TRUE(success); }
    );
    auto sub2 = event_system->subscribe(
        "TestEvent",
        [&call_count](std::string, std::string) { call_count.fetch_add(1); },
        [](bool success) { EXPECT_TRUE(success); }
    );
    
    // Publish the event.
    event_system->publish("TestEvent", "Hello", [](bool success) { EXPECT_TRUE(success); });
    
    // Wait until both callbacks are invoked or timeout occurs.
    auto start = std::chrono::steady_clock::now();
    while(call_count.load() < 2 &&
          (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        detail::get_global_io_context().poll();
    }
    
    EXPECT_EQ(call_count.load(), 2);
}
