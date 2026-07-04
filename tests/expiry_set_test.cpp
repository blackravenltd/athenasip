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
#include "expiry_set.h" 
#include "global_io_context.h"

#include "helpers/process_io_context_for_helper.h"

using namespace athenasip;

// Test that adding an item immediately makes it present.
TEST(ExpirySetTest, AddAndContains) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("item1", 10);
  
  // Immediately, the item should be present.
  EXPECT_TRUE(expirySet->contains("item1"));
  
  // And operator[] should return a valid pointer.
  EXPECT_NE(expirySet->operator[]("item1"), nullptr);
}

// Test that an item is automatically removed after expiry.
TEST(ExpirySetTest, AutoRemoveAfterExpiry) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  // Add an item with a short expiry (e.g., 50ms).
  expirySet->add("temp", 10);
  EXPECT_TRUE(expirySet->contains("temp"));
  
  // Process the I/O context for a duration longer than the expiry.
  process_io_context_for(std::chrono::milliseconds(15));
  
  // The item should have been removed.
  EXPECT_FALSE(expirySet->contains("temp"));
  EXPECT_EQ(expirySet->operator[]("temp"), nullptr);
}

// Test that manually removing an item cancels its delayed task.
TEST(ExpirySetTest, RemoveCancelsTask) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("cancelItem", 10);
  EXPECT_TRUE(expirySet->contains("cancelItem"));
  
  // Remove the item explicitly.
  expirySet->remove("cancelItem");
  EXPECT_FALSE(expirySet->contains("cancelItem"));
  EXPECT_EQ(expirySet->operator[]("cancelItem"), nullptr);
  
  // Process I/O context to ensure any pending delayed task does not re-add it.
  process_io_context_for(std::chrono::milliseconds(15));
  EXPECT_FALSE(expirySet->contains("cancelItem"));
}

// Test that adding the same item twice replaces the previous expiration timer.
TEST(ExpirySetTest, ReplaceExistingTask) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  // Add an item with a relatively long expiry.
  expirySet->add("dup", 20);
  EXPECT_TRUE(expirySet->contains("dup"));
  
  // Add the same item with a shorter expiry. This should cancel the previous task.
  expirySet->add("dup", 10);
  EXPECT_TRUE(expirySet->contains("dup"));
  
  // Process I/O context for a duration longer than the short expiry.
  process_io_context_for(std::chrono::milliseconds(15));
  
  // Now, the item should have been removed.
  EXPECT_FALSE(expirySet->contains("dup"));
  EXPECT_EQ(expirySet->operator[]("dup"), nullptr);
}
