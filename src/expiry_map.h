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
  // Adds or replaces an entry that removes itself after expiryMs milliseconds.
  void add(const K& key, const V& value, uint32_t expiryMs) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      remove_internal(key);
    }

    // The task holds a shared_ptr, keeping the map alive until it fires.
    auto self = this->shared_from_this();
    auto task = [self, key]() -> int {
      self->remove(key);
      return 0;  // The returned value is unused.
    };

    auto delayedTask = DelayedTask<int>::schedule(task, expiryMs);

    {
      std::lock_guard<std::mutex> lock(_mutex);
      _entries[key] = std::make_pair(value, delayedTask);
    }
  }

  bool contains(const K& key) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries.find(key) != _entries.end();
  }

  void remove(const K& key) {
    std::lock_guard<std::mutex> lock(_mutex);
    remove_internal(key);
  }

  // Returns a copy of the value, or a default-constructed V if the key is absent.
  V operator[](const K& key) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _entries.find(key);
    if (it != _entries.end()) {
      return it->second.first;
    }
    return V{};
  }

 protected:
  // Must be called with _mutex held.
  void remove_internal(const K& key) {
    auto it = _entries.find(key);
    if (it != _entries.end()) {
      it->second.second->cancel();
      _entries.erase(it);
    }
  }

  std::unordered_map<K, std::pair<V, std::shared_ptr<DelayedTask<int>>>> _entries;

  mutable std::mutex _mutex;
};

}  // namespace athenasip
