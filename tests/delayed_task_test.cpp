//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <stdexcept>
#include "delayed_task.h"
#include "global_io_context.h"

#include "helpers/process_io_context_for_helper.h"

using namespace athenasip;

TEST(DelayedTaskTest, ExecutesTask) {
  std::atomic<int> testInt;

  testInt = 1;

  auto task = DelayedTask<int>::schedule([&testInt]() -> int { 
    testInt = 24;
    return 42; 
  }, 10);

  EXPECT_EQ(testInt, 1);

  process_io_context_for(std::chrono::milliseconds(20));

  EXPECT_TRUE(task->has_executed());
  EXPECT_EQ(task->result(), 42);
  EXPECT_EQ(testInt, 24);
}

TEST(DelayedTaskTest, CancelPreventsExecution) {
  auto task = DelayedTask<int>::schedule([]() -> int { return 99; }, 10);

  bool cancelled = task->cancel();
  EXPECT_TRUE(cancelled);

  process_io_context_for(std::chrono::milliseconds(15));

  // A cancelled task counts as executed but has no result.
  EXPECT_TRUE(task->has_executed());
  EXPECT_THROW(task->result(), std::runtime_error);
}

TEST(DelayedTaskTest, ThrowsIfNotExecutedYet) {
  auto task = DelayedTask<int>::schedule([]() -> int { return 123; }, 50);

  EXPECT_THROW(task->result(), std::runtime_error);

  task->cancel();
}

TEST(DelayedTaskTest, MultipleCancelCalls) {
  auto task = DelayedTask<int>::schedule([]() -> int { return 5; }, 10);

  bool cancelled1 = task->cancel();
  bool cancelled2 = task->cancel();

  EXPECT_TRUE(cancelled1);
  EXPECT_FALSE(cancelled2);
}
