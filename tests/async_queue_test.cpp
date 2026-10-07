//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "async_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using athenasip::AsyncQueue;

namespace {

// Delivery is posted to the global io_context, which runs on another thread, so tests poll for it.
template <typename Predicate>
bool eventually(Predicate done, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;

  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  return done();
}

}  // namespace

// AsyncQueue sits between the UDP server's thread, which pushes datagrams, and the Core strand,
// where a channel registers its reads.

TEST(AsyncQueueTest, AnItemWaitingIsGivenToTheNextReaderToAsk) {
  AsyncQueue<int> queue;
  std::atomic<int> got{0};

  queue.push(7);
  queue.on_item_once([&got](int item) { got.store(item); });

  EXPECT_TRUE(eventually([&]() { return got.load() == 7; }));
}

TEST(AsyncQueueTest, AReaderWaitingIsGivenTheNextItemToArrive) {
  AsyncQueue<int> queue;
  std::atomic<int> got{0};

  queue.on_item_once([&got](int item) { got.store(item); });
  queue.push(9);

  EXPECT_TRUE(eventually([&]() { return got.load() == 9; }));
}

// Delivery is FIFO: reordering would put a CANCEL ahead of its INVITE.
TEST(AsyncQueueTest, ItemsComeOutInTheOrderTheyWentIn) {
  AsyncQueue<int> queue;

  std::mutex mutex;
  std::vector<int> seen;

  for (int i = 0; i < 3; ++i) {
    queue.on_item_once([&](int item) {
      std::lock_guard<std::mutex> lock(mutex);
      seen.push_back(item);
    });
  }

  queue.push(1);
  queue.push(2);
  queue.push(3);

  EXPECT_TRUE(eventually([&]() {
    std::lock_guard<std::mutex> lock(mutex);
    return seen.size() == 3;
  }));

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

// on_item_once delivers exactly one item; the channel re-registers from its completion handler.
TEST(AsyncQueueTest, OneReaderTakesOneItem) {
  AsyncQueue<int> queue;
  std::atomic<int> deliveries{0};

  queue.on_item_once([&deliveries](int) { deliveries.fetch_add(1); });

  queue.push(1);
  queue.push(2);
  queue.push(3);

  EXPECT_TRUE(eventually([&]() { return deliveries.load() >= 1; }));

  // Long enough that a second delivery would have happened by now.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(deliveries.load(), 1);
}

TEST(AsyncQueueTest, ItemsWithNobodyToTakeThemWaitRatherThanBeingDropped) {
  AsyncQueue<std::string> queue;

  queue.push("first");
  queue.push("second");

  std::mutex mutex;
  std::vector<std::string> seen;

  for (int i = 0; i < 2; ++i) {
    queue.on_item_once([&](std::string item) {
      std::lock_guard<std::mutex> lock(mutex);
      seen.push_back(std::move(item));
    });
  }

  EXPECT_TRUE(eventually([&]() {
    std::lock_guard<std::mutex> lock(mutex);
    return seen.size() == 2;
  }));

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(seen, (std::vector<std::string>{"first", "second"}));
}

// Pushing and reading from different threads delivers every item exactly once.
TEST(AsyncQueueTest, NothingIsLostOrDuplicatedUnderConcurrentPushAndRead) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  constexpr int kTotal = kThreads * kPerThread;

  AsyncQueue<int> queue;

  std::mutex mutex;
  std::vector<int> seen;
  std::atomic<int> delivered{0};

  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&queue, t]() {
      for (int i = 0; i < kPerThread; ++i) queue.push((t * kPerThread) + i);
    });

    threads.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        queue.on_item_once([&](int item) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(item);
          }
          delivered.fetch_add(1);
        });
      }
    });
  }

  for (auto& thread : threads) thread.join();

  EXPECT_TRUE(eventually([&]() { return delivered.load() == kTotal; }, std::chrono::seconds(20)));

  std::lock_guard<std::mutex> lock(mutex);

  ASSERT_EQ(seen.size(), static_cast<std::size_t>(kTotal));

  const std::set<int> unique(seen.begin(), seen.end());
  EXPECT_EQ(unique.size(), static_cast<std::size_t>(kTotal));
  EXPECT_EQ(*unique.begin(), 0);
  EXPECT_EQ(*unique.rbegin(), kTotal - 1);
}
