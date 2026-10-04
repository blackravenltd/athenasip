//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "global_io_context.h"

namespace athenasip {

// The clock for delayed work. Injectable, so tests of the RFC 3261 section 17 timers (up
// to 64*T1, 32 seconds) use ManualTimerSource and advance time themselves; production uses
// AsioTimerSource.

class Timer {
 public:
  virtual ~Timer() = default;

  // True when this call cancelled the timer, false when it had already fired or been
  // cancelled.
  virtual bool cancel() = 0;
};

class TimerSource {
 public:
  virtual ~TimerSource() = default;

  virtual std::shared_ptr<Timer> schedule(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
  virtual std::chrono::steady_clock::time_point now() const = 0;
};

// The real clock: an asio steady_timer on an io_context.
class AsioTimerSource : public TimerSource, public std::enable_shared_from_this<AsioTimerSource> {
 public:
  explicit AsioTimerSource(boost::asio::io_context& io_context) : _io_context(io_context) {}

  std::shared_ptr<Timer> schedule(std::chrono::milliseconds delay, std::function<void()> fn) override {
    auto timer = std::make_shared<AsioTimer>(_io_context);
    timer->start(delay, std::move(fn));
    return timer;
  }

  std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::now(); }

 private:
  class AsioTimer : public Timer, public std::enable_shared_from_this<AsioTimer> {
   public:
    explicit AsioTimer(boost::asio::io_context& io_context) : _timer(io_context) {}

    void start(std::chrono::milliseconds delay, std::function<void()> fn) {
      _timer.expires_after(delay);

      auto self = shared_from_this();
      _timer.async_wait([self, fn = std::move(fn)](const boost::system::error_code& ec) {
        if (ec) return;

        bool expected = false;
        if (self->_done.compare_exchange_strong(expected, true) && fn) fn();
      });
    }

    bool cancel() override {
      bool expected = false;
      if (!_done.compare_exchange_strong(expected, true)) return false;

      _timer.cancel();
      return true;
    }

   private:
    boost::asio::steady_timer _timer;
    std::atomic<bool> _done{false};
  };

  boost::asio::io_context& _io_context;
};

// The test clock: nothing fires until advance() is called.
class ManualTimerSource : public TimerSource {
 public:
  std::shared_ptr<Timer> schedule(std::chrono::milliseconds delay, std::function<void()> fn) override {
    auto entry = std::make_shared<Entry>();
    entry->due = _now + delay;
    entry->fn = std::move(fn);

    std::lock_guard<std::mutex> lock(_mutex);
    _entries.push_back(entry);
    return entry;
  }

  std::chrono::steady_clock::time_point now() const override {
    std::lock_guard<std::mutex> lock(_mutex);
    return _now;
  }

  // Moves time forward, stopping at each timer as it falls due, so a callback that re-arms
  // (as Timer A does) sees the time its own timer fired.
  void advance(std::chrono::milliseconds by) {
    std::chrono::steady_clock::time_point target;

    {
      std::lock_guard<std::mutex> lock(_mutex);
      target = _now + by;
    }

    while (true) {
      std::shared_ptr<Entry> next;

      {
        std::lock_guard<std::mutex> lock(_mutex);

        // Drop entries that have fired or been cancelled.
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(), [](const auto& entry) { return entry->fired || entry->cancelled; }), _entries.end());

        for (const auto& entry : _entries) {
          if (entry->due > target) continue;
          if (!next || entry->due < next->due) next = entry;
        }

        if (!next) {
          _now = target;
          return;
        }

        // Step time to this timer, never backwards.
        if (next->due > _now) _now = next->due;
      }

      bool expected = false;
      if (next->fired.compare_exchange_strong(expected, true) && next->fn) next->fn();
    }
  }

  std::size_t pending() const {
    std::lock_guard<std::mutex> lock(_mutex);

    std::size_t count = 0;
    for (const auto& entry : _entries) {
      if (!entry->fired && !entry->cancelled) count++;
    }
    return count;
  }

 private:
  struct Entry : public Timer {
    std::chrono::steady_clock::time_point due;
    std::function<void()> fn;
    std::atomic<bool> fired{false};
    std::atomic<bool> cancelled{false};

    bool cancel() override {
      if (fired.load()) return false;

      bool expected = false;
      return cancelled.compare_exchange_strong(expected, true);
    }
  };

  mutable std::mutex _mutex;
  std::chrono::steady_clock::time_point _now{std::chrono::steady_clock::now()};
  std::vector<std::shared_ptr<Entry>> _entries;
};

// The process-wide real source, on the global io_context.
inline std::shared_ptr<TimerSource>& default_timer_source() {
  static std::shared_ptr<TimerSource> source = std::make_shared<AsioTimerSource>(detail::get_global_io_context());
  return source;
}

}  // namespace athenasip
