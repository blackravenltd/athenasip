//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

#include "global_io_context.h"

namespace athenasip {

// Hands items from one thread to readers on another: each item goes to exactly one reader,
// in arrival order. On the UDP path the server's io_context thread pushes and the Core
// strand reads.
//
// One mutex is held across both the pairing and the post, so handlers are queued in arrival
// order; posting outside the lock could deliver a CANCEL before its INVITE. asio::post never
// runs a handler inline, so nothing re-enters while the lock is held.
template <typename T>
class AsyncQueue {
  using CallbackFn = std::function<void(T item)>;

 public:
  AsyncQueue() : _io_context(detail::get_global_io_context()) {}

  void push(T item) {
    std::lock_guard<std::mutex> lock(_mutex);

    _items.push(std::move(item));
    _pair_up();
  }

  void on_item_once(CallbackFn callback) {
    std::lock_guard<std::mutex> lock(_mutex);

    // A reader registered after close would wait for ever, holding whatever it captured.
    if (_closed) return;

    _callbacks.push(std::move(callback));
    _pair_up();
  }

  // Stops delivery and drops the waiting readers. A pending callback owns its captures (on
  // the UDP path, a shared_ptr to the channel), and nothing else would ever release them.
  void close() {
    std::queue<CallbackFn> callbacks;
    std::queue<T> items;

    {
      std::lock_guard<std::mutex> lock(_mutex);

      _closed = true;

      // Destroyed outside the lock: a callback's destructor can run arbitrary code.
      callbacks.swap(_callbacks);
      items.swap(_items);
    }
  }

 protected:
  // Called with _mutex held.
  void _pair_up() {
    while (!_items.empty() && !_callbacks.empty()) {
      auto callback = std::move(_callbacks.front());
      _callbacks.pop();

      auto item = std::move(_items.front());
      _items.pop();

      boost::asio::post(_io_context, [callback = std::move(callback), item = std::move(item)]() mutable { callback(std::move(item)); });
    }
  }

  boost::asio::io_context& _io_context;

  std::mutex _mutex;
  std::queue<CallbackFn> _callbacks;
  std::queue<T> _items;
  bool _closed = false;
};

}  // namespace athenasip
