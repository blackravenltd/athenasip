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

  // Schedule a task that returns 42 after 50ms.
  auto task = DelayedTask<int>::schedule([&testInt]() -> int { 
    testInt = 24;
    return 42; 
  }, 10);

  // Should not be touched yet
  EXPECT_EQ(testInt, 1);

  // Wait sufficiently for the task to execute.
  process_io_context_for(std::chrono::milliseconds(20));

  // The task should have executed.
  EXPECT_TRUE(task->has_executed());
  EXPECT_EQ(task->result(), 42);
  EXPECT_EQ(testInt, 24);
}

TEST(DelayedTaskTest, CancelPreventsExecution) {
  // Schedule a task that would return 99 after 10ms.
  auto task = DelayedTask<int>::schedule([]() -> int { return 99; }, 10);

  // Immediately cancel the task.
  bool cancelled = task->cancel();
  EXPECT_TRUE(cancelled);

  // Wait long enough that the timer would have fired if not cancelled.
  process_io_context_for(std::chrono::milliseconds(15));

  // The task should be marked as executed (by cancellation).
  EXPECT_TRUE(task->has_executed());
  // Accessing the result should throw because the task was cancelled.
  EXPECT_THROW(task->result(), std::runtime_error);
}

TEST(DelayedTaskTest, ThrowsIfNotExecutedYet) {
  // Schedule a task with a long delay.
  auto task = DelayedTask<int>::schedule([]() -> int { return 123; }, 50);

  // Immediately calling result() should throw, as the task hasn't executed.
  EXPECT_THROW(task->result(), std::runtime_error);

  // Cancel the task to avoid later execution.
  task->cancel();
}

TEST(DelayedTaskTest, MultipleCancelCalls) {
  // Schedule a task.
  auto task = DelayedTask<int>::schedule([]() -> int { return 5; }, 10);

  // First cancel should succeed.
  bool cancelled1 = task->cancel();
  // A second call to cancel should return false.
  bool cancelled2 = task->cancel();

  EXPECT_TRUE(cancelled1);
  EXPECT_FALSE(cancelled2);
}
