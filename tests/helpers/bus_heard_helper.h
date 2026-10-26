//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// What a test heard on a topic. The bus delivers on its own thread and may deliver after the test has looked, so it is
// held by shared pointer from the callback (a late delivery lands here, never in a test's dead local) and read under
// a lock, with a wait for the count a test expects.
struct BusHeard {
  std::mutex mutex;
  std::vector<std::string> messages;

  // A callback for SyncEventSystem::subscribe that keeps this alive.
  static auto into(const std::shared_ptr<BusHeard>& heard) {
    return [heard](const std::string&, const std::string& message) {
      std::lock_guard<std::mutex> lock(heard->mutex);
      heard->messages.push_back(message);
    };
  }

  // Waits up to two seconds for at least `count`, then returns what was heard.
  std::vector<std::string> wait_for(std::size_t count) {
    for (int i = 0; i < 400; ++i) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (messages.size() >= count) return messages;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lock(mutex);
    return messages;
  }
};
