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

// Delivery is a post onto the global io_context, which runs on its own thread, so a
// test waits for it rather than expecting it to have happened already.
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

// This is the queue between a UDP datagram arriving on the server's thread and the
// channel asking to read on the Core strand. Those are two different threads, and
// everything below is a statement about what the queue owes them.

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

// Datagrams are not interchangeable and neither are the requests in them. A queue that
// reordered would deliver a CANCEL before the INVITE it cancels.
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

// One reader takes one item. The name says once and the caller depends on it: a channel
// asks again from inside its own completion, so a second delivery would be a second
// read nobody started.
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

// The whole point of this queue is that the two ends are on different threads: a
// datagram is pushed from the UDP server's thread and a read is registered from the
// Core strand. Every item has to come out exactly once, and neither queue may be
// touched by two threads at a time.
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

  // Every value once and no value twice, which is what "lost or duplicated" means.
  const std::set<int> unique(seen.begin(), seen.end());
  EXPECT_EQ(unique.size(), static_cast<std::size_t>(kTotal));
  EXPECT_EQ(*unique.begin(), 0);
  EXPECT_EQ(*unique.rbegin(), kTotal - 1);
}
