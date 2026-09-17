//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "delayed_task.h"

namespace athenasip {

template <typename T>
class ExpirySet : public std::enable_shared_from_this<ExpirySet<T>> {
 public:
  // Adds the item with an expiry in expiryMs milliseconds.
  // If the item is already in the set, its previous timer is cancelled.
  void add(const T& item, uint32_t expiryMs) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      // Cancel any existing expiry for this item.
      remove_internal(item);
    }

    // Create a lambda that will be invoked when the delayed task expires.
    // It captures a shared pointer to this ExpirySet and the item by value.
    auto self = this->shared_from_this();

    // Create and schedule the delayed task.
    auto delayedTask = DelayedTask<int>::schedule(
        [self, item]() {
          // Remove the item from the set once the timer expires.
          self->remove(item);
          return 0;  // the result is unused.
        },
        expiryMs);

    // Insert the new delayed task.
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _timeouts[item] = delayedTask;
    }
  }

  // Returns true if the item is present in the set.
  bool contains(const T& item) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _timeouts.find(item) != _timeouts.end();
  }

  // Removes the item from the set.
  // If an expiry task exists, it is cancelled.
  void remove(const T& item) {
    std::lock_guard<std::mutex> lock(_mutex);
    remove_internal(item);
  }

  // Const version of operator[] to access the delayed task associated with an item.
  // Returns nullptr if the item is not present.
  std::shared_ptr<DelayedTask<int>> operator[](const T& item) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      return it->second;
    }
    return nullptr;
  }

  // Non-const version of operator[] to access the delayed task associated with an item.
  // Returns nullptr if the item is not present.
  std::shared_ptr<DelayedTask<int>> operator[](const T& item) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      return it->second;
    }
    return nullptr;
  }

 protected:
  // Internal remove function, must be called with _mutex locked.
  void remove_internal(const T& item) {
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      // Cancel the associated delayed task.
      it->second->cancel();
      _timeouts.erase(it);
    }
  }

  // Map from items to their corresponding delayed expiration tasks.
  std::unordered_map<T, std::shared_ptr<DelayedTask<int>>> _timeouts;

  // Mutex to guard access to _timeouts.
  mutable std::mutex _mutex;
};

}  // namespace athenasip
