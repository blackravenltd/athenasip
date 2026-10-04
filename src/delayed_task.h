//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "global_io_context.h"
#include "timer_source.h"

namespace athenasip {

// Runs a task once after a delay, unless cancelled first.
template <typename T>
class DelayedTask : public std::enable_shared_from_this<DelayedTask<T>> {
 public:
  using Task = std::function<T()>;

  // The timer source defaults to the real clock; tests pass a ManualTimerSource.
  static std::shared_ptr<DelayedTask<T>> schedule(Task task, int delay_ms, std::shared_ptr<TimerSource> timers = default_timer_source()) {
    auto instance = std::shared_ptr<DelayedTask<T>>(new DelayedTask<T>(std::move(task), delay_ms, std::move(timers)));

    instance->startTimer();
    return instance;
  }

  // Returns true if this call cancelled the task, false if it had already run or been
  // cancelled.
  bool cancel() {
    bool expected = false;
    if (_has_executed.compare_exchange_strong(expected, true)) {
      if (_timer) _timer->cancel();
      return true;
    }
    return false;
  }

  // True once the task has run or been cancelled.
  bool has_executed() const { return _has_executed.load(); }

  // The task's return value. Throws if it has not run or was cancelled.
  T result() const {
    if (!_has_executed.load()) {
      throw std::runtime_error("Task has not executed yet.");
    }
    if (!_result.has_value()) {
      throw std::runtime_error("Task was canceled or no result available.");
    }
    return *_result;
  }

  ~DelayedTask() = default;

 private:
  DelayedTask(Task task, int delay_ms, std::shared_ptr<TimerSource> timers)
      : _task(std::move(task)), _delay_ms(delay_ms), _timers(std::move(timers)), _has_executed(false) {}

  void startTimer() {
    // Keep this alive until the timer fires.
    auto self = this->shared_from_this();

    _timer = _timers->schedule(std::chrono::milliseconds(_delay_ms), [self]() {
      bool expected = false;
      if (self->_has_executed.compare_exchange_strong(expected, true)) {
        self->_result = self->_task();
      }
    });
  }

  Task _task;
  int _delay_ms;
  std::shared_ptr<TimerSource> _timers;
  std::shared_ptr<Timer> _timer;
  std::optional<T> _result;
  std::atomic<bool> _has_executed;
};

}  // namespace athenasip
