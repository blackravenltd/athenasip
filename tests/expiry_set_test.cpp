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

TEST(ExpirySetTest, AddAndContains) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("item1", 10);
  
  EXPECT_TRUE(expirySet->contains("item1"));
  
  EXPECT_NE(expirySet->operator[]("item1"), nullptr);
}

TEST(ExpirySetTest, AutoRemoveAfterExpiry) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("temp", 10);
  EXPECT_TRUE(expirySet->contains("temp"));
  
  process_io_context_for(std::chrono::milliseconds(15));
  
  EXPECT_FALSE(expirySet->contains("temp"));
  EXPECT_EQ(expirySet->operator[]("temp"), nullptr);
}

// Removing an item cancels its expiry timer.
TEST(ExpirySetTest, RemoveCancelsTask) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("cancelItem", 10);
  EXPECT_TRUE(expirySet->contains("cancelItem"));
  
  expirySet->remove("cancelItem");
  EXPECT_FALSE(expirySet->contains("cancelItem"));
  EXPECT_EQ(expirySet->operator[]("cancelItem"), nullptr);
  
  process_io_context_for(std::chrono::milliseconds(15));
  EXPECT_FALSE(expirySet->contains("cancelItem"));
}

// Adding an item again replaces its expiry timer.
TEST(ExpirySetTest, ReplaceExistingTask) {
  auto expirySet = std::make_shared<ExpirySet<std::string>>();
  
  expirySet->add("dup", 20);
  EXPECT_TRUE(expirySet->contains("dup"));
  
  expirySet->add("dup", 10);
  EXPECT_TRUE(expirySet->contains("dup"));
  
  process_io_context_for(std::chrono::milliseconds(15));
  
  EXPECT_FALSE(expirySet->contains("dup"));
  EXPECT_EQ(expirySet->operator[]("dup"), nullptr);
}
