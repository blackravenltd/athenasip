//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace athenasip::api {

// A response that stays open after its headers, for Server-Sent Events. The HTTP session owns the connection; a
// handler holds this to add to the body until the client goes.
class ResponseStream {
 public:
  // Sends more of the body. False once the client has gone. Any thread.
  bool write(std::string data) {
    if (!_open || !_send) return false;
    _send(std::move(data));
    return true;
  }

  bool open() const { return _open; }

  // Runs once when the client goes or the listener stops; at once if it already has.
  void on_close(std::function<void()> fn) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_open) {
      lock.unlock();
      fn();
      return;
    }
    _on_close.push_back(std::move(fn));
  }

  // For the session.
  void attach(std::function<void(std::string)> send) { _send = std::move(send); }
  void close() {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (!_open.exchange(false)) return;
      callbacks.swap(_on_close);
    }
    for (auto& callback : callbacks) callback();
  }

 private:
  std::atomic<bool> _open{true};
  std::function<void(std::string)> _send;
  std::mutex _mutex;
  std::vector<std::function<void()>> _on_close;
};

using StreamStart = std::function<void(std::shared_ptr<ResponseStream>)>;

// Where a route leaves a stream for the session that will write its response, keyed by that response.
class StreamRegistry {
 public:
  void add(const void* response, StreamStart start) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending[response] = std::move(start);
  }

  StreamStart take(const void* response) {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _pending.find(response);
    if (found == _pending.end()) return nullptr;
    auto start = std::move(found->second);
    _pending.erase(found);
    return start;
  }

 private:
  std::mutex _mutex;
  std::map<const void*, StreamStart> _pending;
};

}  // namespace athenasip::api
