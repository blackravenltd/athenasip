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
  // Adds an item that removes itself after expiryMs milliseconds, replacing any existing
  // expiry.
  void add(const T& item, uint32_t expiryMs) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      remove_internal(item);
    }

    // The task holds a shared_ptr, keeping the set alive until it fires.
    auto self = this->shared_from_this();

    auto delayedTask = DelayedTask<int>::schedule(
        [self, item]() {
          self->remove(item);
          return 0;  // the result is unused.
        },
        expiryMs);

    {
      std::lock_guard<std::mutex> lock(_mutex);
      _timeouts[item] = delayedTask;
    }
  }

  bool contains(const T& item) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _timeouts.find(item) != _timeouts.end();
  }

  void remove(const T& item) {
    std::lock_guard<std::mutex> lock(_mutex);
    remove_internal(item);
  }

  // The item's expiry task, or nullptr if the item is absent.
  std::shared_ptr<DelayedTask<int>> operator[](const T& item) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      return it->second;
    }
    return nullptr;
  }

  std::shared_ptr<DelayedTask<int>> operator[](const T& item) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      return it->second;
    }
    return nullptr;
  }

 protected:
  // Must be called with _mutex held.
  void remove_internal(const T& item) {
    auto it = _timeouts.find(item);
    if (it != _timeouts.end()) {
      it->second->cancel();
      _timeouts.erase(it);
    }
  }

  std::unordered_map<T, std::shared_ptr<DelayedTask<int>>> _timeouts;

  mutable std::mutex _mutex;
};

}  // namespace athenasip
