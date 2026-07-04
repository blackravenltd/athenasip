//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <string>
#include "expiry_map.h"
#include "global_io_context.h"

#include "helpers/process_io_context_for_helper.h"

using namespace athenasip;

// Test that adding an entry immediately reflects in contains() and operator[].
TEST(ExpiryMapTest, AddEntryAndContains) {
    // Create an ExpiryMap with key type string and value type int.
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    // Add an entry that expires in 10ms.
    expiryMap->add("key1", 42, 10);

    // Immediately, the map should contain the key.
    EXPECT_TRUE(expiryMap->contains("key1"));
    EXPECT_EQ((*expiryMap)["key1"], 42);
}

// Test that an entry is automatically removed after its expiry.
TEST(ExpiryMapTest, AutoRemoveAfterExpiry) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    // Add an entry with a short expiry (10ms).
    expiryMap->add("key2", 99, 10);
    // Immediately, the entry exists.
    EXPECT_TRUE(expiryMap->contains("key2"));

    // Process the I/O context for a duration longer than the expiry.
    process_io_context_for(std::chrono::milliseconds(15));

    // Now, the entry should be gone.
    EXPECT_FALSE(expiryMap->contains("key2"));
    // Operator[] returns default value when key is absent.
    EXPECT_EQ((*expiryMap)["key2"], int{});
}

// Test that calling remove cancels the delayed task and the entry is removed.
TEST(ExpiryMapTest, RemoveCancelsTask) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    // Add an entry with a longer expiry.
    expiryMap->add("key3", 123, 20);
    EXPECT_TRUE(expiryMap->contains("key3"));

    // Remove the entry manually.
    expiryMap->remove("key3");
    EXPECT_FALSE(expiryMap->contains("key3"));
    // Operator[] should return default value.
    EXPECT_EQ((*expiryMap)["key3"], int{});

    // Process I/O context to allow any pending delayed task to run.
    process_io_context_for(std::chrono::milliseconds(50));
    // The entry remains absent.
    EXPECT_FALSE(expiryMap->contains("key3"));
}

// Test that adding the same key twice replaces the previous entry.
TEST(ExpiryMapTest, ReplaceEntry) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    // Add an entry with key "dup" and value 10.
    expiryMap->add("dup", 10, 20);
    EXPECT_TRUE(expiryMap->contains("dup"));
    EXPECT_EQ((*expiryMap)["dup"], 10);

    // Add the same key with a different value (20) and same expiry.
    expiryMap->add("dup", 20, 20);
    // The new value should be present.
    EXPECT_TRUE(expiryMap->contains("dup"));
    EXPECT_EQ((*expiryMap)["dup"], 20);

    // Process I/O context beyond expiry.
    process_io_context_for(std::chrono::milliseconds(30));
    // The key should now be expired/removed.
    EXPECT_FALSE(expiryMap->contains("dup"));
}

