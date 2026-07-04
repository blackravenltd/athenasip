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

#include "global_io_context.h"

namespace athenasip {

template <typename T>
class AsyncQueue {
  using CallbackFn = std::function<void(T item)>;

 public:
  AsyncQueue() : _io_context(detail::getGlobalIOContext()) {}

  void push(T item) {
    {
      std::lock_guard<std::mutex> lock(_items_mutex);
      _items.push(item);
    }
    _check_item();
  }

  void on_item_once(CallbackFn callback) {
    {
      std::lock_guard<std::mutex> lock(_callback_mutex);
      _callbacks.push(callback);
    }

    _check_item();
  }

 protected:
  void _check_item() {
    while (_items.size() > 0 && _callbacks.size()) {
      auto callback = _callbacks.front();
      _callbacks.pop();
      auto item = _items.front();
      _items.pop();
      boost::asio::post(_io_context, [item, callback]() { callback(item); });
    }
  }

  boost::asio::io_context& _io_context;
  std::queue<CallbackFn> _callbacks;
  std::mutex _callback_mutex;
  std::queue<T> _items;
  std::mutex _items_mutex;
};

}  // namespace athenasip
