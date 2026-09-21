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

// A handover between two threads: on the UDP path a datagram is pushed from the
// server's own io_context thread and a read is registered from the Core strand. Items
// wait for readers, readers wait for items, and each item goes to exactly one reader in
// the order the items arrived.
//
// One mutex covers both queues, and it is held across the pairing and the post. Holding
// it that long is deliberate: the order handlers are queued in is the order the
// datagrams arrived in, and a drain that paired under the lock and posted outside it
// would let two threads interleave their posts and deliver a CANCEL before its INVITE.
// asio::post never runs a handler inline, so nothing re-enters this class while the
// lock is held.
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

    _callbacks.push(std::move(callback));
    _pair_up();
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
};

}  // namespace athenasip
