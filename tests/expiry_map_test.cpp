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

TEST(ExpiryMapTest, AddEntryAndContains) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    expiryMap->add("key1", 42, 10);

    EXPECT_TRUE(expiryMap->contains("key1"));
    EXPECT_EQ((*expiryMap)["key1"], 42);
}

// An expired key reads as the default value.
TEST(ExpiryMapTest, AutoRemoveAfterExpiry) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    expiryMap->add("key2", 99, 10);
    EXPECT_TRUE(expiryMap->contains("key2"));

    process_io_context_for(std::chrono::milliseconds(15));

    EXPECT_FALSE(expiryMap->contains("key2"));
    EXPECT_EQ((*expiryMap)["key2"], int{});
}

// Removing an entry cancels its expiry timer.
TEST(ExpiryMapTest, RemoveCancelsTask) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    expiryMap->add("key3", 123, 20);
    EXPECT_TRUE(expiryMap->contains("key3"));

    expiryMap->remove("key3");
    EXPECT_FALSE(expiryMap->contains("key3"));
    EXPECT_EQ((*expiryMap)["key3"], int{});

    process_io_context_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(expiryMap->contains("key3"));
}

// Adding a key again replaces its value and its expiry timer.
TEST(ExpiryMapTest, ReplaceEntry) {
    auto expiryMap = std::make_shared<ExpiryMap<std::string, int>>();

    expiryMap->add("dup", 10, 20);
    EXPECT_TRUE(expiryMap->contains("dup"));
    EXPECT_EQ((*expiryMap)["dup"], 10);

    expiryMap->add("dup", 20, 20);
    EXPECT_TRUE(expiryMap->contains("dup"));
    EXPECT_EQ((*expiryMap)["dup"], 20);

    process_io_context_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(expiryMap->contains("dup"));
}

