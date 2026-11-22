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
#include <thread>
#include <vector>

#include "delayed_task.h"
#include "timer_source.h"

using namespace athenasip;
using namespace std::chrono_literals;

TEST(ManualTimerSourceTest, NothingFiresUntilTimeMoves) {
  auto timers = std::make_shared<ManualTimerSource>();
  bool fired = false;

  timers->schedule(500ms, [&fired]() { fired = true; });

  EXPECT_FALSE(fired);
  EXPECT_EQ(timers->pending(), 1u);

  timers->advance(499ms);
  EXPECT_FALSE(fired);

  timers->advance(1ms);
  EXPECT_TRUE(fired);
  EXPECT_EQ(timers->pending(), 0u);
}

// A timer that has fired or been cancelled lets go of its function, as an asio timer lets go of its handler: what
// the function holds is often what holds the timer, and kept it would be a cycle that is never freed.
TEST(ManualTimerSourceTest, ATimerDoneWithLetsGoOfItsFunction) {
  auto timers = std::make_shared<ManualTimerSource>();

  auto fires = std::make_shared<int>(0);
  auto cancelled = std::make_shared<int>(0);
  std::weak_ptr<int> fires_held = fires;
  std::weak_ptr<int> cancelled_held = cancelled;

  auto first = timers->schedule(100ms, [fires]() { (*fires)++; });
  auto second = timers->schedule(100ms, [cancelled]() { (*cancelled)++; });
  fires.reset();
  cancelled.reset();

  EXPECT_TRUE(second->cancel());
  EXPECT_TRUE(cancelled_held.expired()) << "a cancelled timer still holds its function";

  timers->advance(100ms);
  EXPECT_TRUE(fires_held.expired()) << "a fired timer still holds its function";
}

// The clock is shared between a test's thread and the strand it drives, so scheduling on one while the other moves
// time is safe. ThreadSanitizer is what proves it: a plain build passes either way.
TEST(ManualTimerSourceTest, ScheduleAndAdvanceFromTwoThreads) {
  auto timers = std::make_shared<ManualTimerSource>();
  std::atomic<int> fired{0};

  std::thread scheduler([&]() {
    for (int i = 0; i < 1000; ++i) timers->schedule(1ms, [&fired]() { fired++; });
  });
  for (int i = 0; i < 1000; ++i) timers->advance(1ms);
  scheduler.join();

  timers->advance(10ms);
  EXPECT_EQ(fired.load(), 1000);
}

// At a test's end the clock stops, as an io_context destroyed takes its handlers with it.
TEST(ManualTimerSourceTest, CancelAllStopsEveryPendingTimer) {
  auto timers = std::make_shared<ManualTimerSource>();
  auto held = std::make_shared<int>(0);
  std::weak_ptr<int> weak = held;
  bool fired = false;

  timers->schedule(100ms, [held, &fired]() { fired = true; });
  held.reset();

  timers->cancel_all();
  EXPECT_EQ(timers->pending(), 0u);
  EXPECT_TRUE(weak.expired());

  timers->advance(200ms);
  EXPECT_FALSE(fired);
}

TEST(ManualTimerSourceTest, FiresInDueOrder) {
  auto timers = std::make_shared<ManualTimerSource>();
  std::vector<int> order;

  timers->schedule(300ms, [&order]() { order.push_back(3); });
  timers->schedule(100ms, [&order]() { order.push_back(1); });
  timers->schedule(200ms, [&order]() { order.push_back(2); });

  timers->advance(1s);

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(ManualTimerSourceTest, CancelStopsATimer) {
  auto timers = std::make_shared<ManualTimerSource>();
  bool fired = false;

  auto timer = timers->schedule(100ms, [&fired]() { fired = true; });
  EXPECT_TRUE(timer->cancel());

  // A second cancel reports nothing cancelled.
  EXPECT_FALSE(timer->cancel());

  timers->advance(1s);
  EXPECT_FALSE(fired);
}

TEST(ManualTimerSourceTest, CancelAfterFiringReportsFalse) {
  auto timers = std::make_shared<ManualTimerSource>();

  auto timer = timers->schedule(100ms, []() {});
  timers->advance(100ms);

  EXPECT_FALSE(timer->cancel());
}

// A callback may schedule more work, as a retransmission timer does; anything falling
// due within the same advance runs in that call.
TEST(ManualTimerSourceTest, CallbacksMayScheduleMoreWork) {
  auto timers = std::make_shared<ManualTimerSource>();
  int fires = 0;

  std::function<void()> rearm = [&]() {
    fires++;
    if (fires < 5) timers->schedule(100ms, rearm);
  };

  timers->schedule(100ms, rearm);
  timers->advance(1s);

  EXPECT_EQ(fires, 5);
}

// RFC 3261 timer B is 64*T1, 32 seconds at the default T1; advancing the clock is instant.
TEST(ManualTimerSourceTest, ATimerBLengthWaitCostsNothing) {
  auto timers = std::make_shared<ManualTimerSource>();
  bool timed_out = false;

  const auto timer_b = 64 * 500ms;
  timers->schedule(timer_b, [&timed_out]() { timed_out = true; });

  const auto started = std::chrono::steady_clock::now();
  timers->advance(timer_b);
  const auto real_elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(timed_out);
  EXPECT_LT(real_elapsed, 1s);
}

TEST(DelayedTaskTest, RunsOnAManualTimerSource) {
  auto timers = std::make_shared<ManualTimerSource>();

  auto task = DelayedTask<int>::schedule([]() { return 42; }, 32000, timers);

  EXPECT_FALSE(task->has_executed());
  EXPECT_THROW(task->result(), std::runtime_error);

  timers->advance(32s);

  EXPECT_TRUE(task->has_executed());
  EXPECT_EQ(task->result(), 42);
}

TEST(DelayedTaskTest, CancelOnAManualTimerSourceStopsIt) {
  auto timers = std::make_shared<ManualTimerSource>();
  bool ran = false;

  auto task = DelayedTask<int>::schedule(
      [&ran]() {
        ran = true;
        return 1;
      },
      1000, timers);

  EXPECT_TRUE(task->cancel());
  EXPECT_FALSE(task->cancel());

  timers->advance(10s);
  EXPECT_FALSE(ran);
}
