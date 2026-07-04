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
#include <utility>

#include "delayed_task.h"

namespace athenasip {

template <typename K, typename V>
class ExpiryMap : public std::enable_shared_from_this<ExpiryMap<K, V>> {
 public:
  // Adds or replaces an entry with the given key and value, expiring after expiryMs milliseconds.
  // If an entry already exists for the key, its expiration timer is canceled and replaced.
  void add(const K& key, const V& value, uint32_t expiryMs) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      removeInternal(key);
    }

    // Capture a shared pointer to this so that the lambda keeps the ExpiryMap alive.
    auto self = this->shared_from_this();
    auto task = [self, key]() -> int {
      // Remove the entry once the timer expires.
      self->remove(key);
      return 0;  // The returned value is unused.
    };

    // Create and schedule the delayed task.
    auto delayedTask = DelayedTask<int>::schedule(task, expiryMs);

    // Insert the new entry along with its delayed task.
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _entries[key] = std::make_pair(value, delayedTask);
    }
  }

  // Returns true if the map contains the specified key.
  bool contains(const K& key) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries.find(key) != _entries.end();
  }

  // Removes the entry for the given key.
  // Cancels the associated delayed task if it exists.
  void remove(const K& key) {
    std::lock_guard<std::mutex> lock(_mutex);
    removeInternal(key);
  }

  // Operator[] returns a copy of the value associated with the key.
  // If the key does not exist, a default-constructed V is returned.
  V operator[](const K& key) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _entries.find(key);
    if (it != _entries.end()) {
      return it->second.first;
    }
    return V{};
  }

 protected:
  // Internal helper to remove an entry. Must be called with _mutex already locked.
  void removeInternal(const K& key) {
    auto it = _entries.find(key);
    if (it != _entries.end()) {
      // Cancel the expiration timer.
      it->second.second->cancel();
      _entries.erase(it);
    }
  }

  // Map storing the key and a pair of value and its corresponding delayed expiration task.
  std::unordered_map<K, std::pair<V, std::shared_ptr<DelayedTask<int>>>> _entries;

  // Mutex to guard access to _entries.
  mutable std::mutex _mutex;
};

}  // namespace athenasip
