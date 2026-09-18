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

// A delayed, cancelable task that runs a std::function<T()> after some milliseconds.
template <typename T>
class DelayedTask : public std::enable_shared_from_this<DelayedTask<T>> {
 public:
  using Task = std::function<T()>;

  // Factory method to create and schedule a delayed task. The timer source defaults to
  // the real clock; tests pass a ManualTimerSource so a 32 second timer costs nothing.
  static std::shared_ptr<DelayedTask<T>> schedule(Task task, int delay_ms, std::shared_ptr<TimerSource> timers = default_timer_source()) {
    auto instance = std::shared_ptr<DelayedTask<T>>(new DelayedTask<T>(std::move(task), delay_ms, std::move(timers)));

    // schedule the async wait
    instance->startTimer();
    return instance;
  }

  // Cancels the task if it hasn't already executed.
  // Returns true if this call actually canceled the task,
  // false if the task was already canceled or already ran.
  bool cancel() {
    bool expected = false;
    // If _has_executed was false, try to mark it true so we know we can't run
    if (_has_executed.compare_exchange_strong(expected, true)) {
      if (_timer) _timer->cancel();  // forcibly cancel the timer
      return true;
    }
    // task was already executed or canceled
    return false;
  }

  // Checks if the task has executed
  bool has_executed() const { return _has_executed.load(); }

  // Gets the result of the lambda, if it has run
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
  // Private constructor (use schedule() factory).
  DelayedTask(Task task, int delay_ms, std::shared_ptr<TimerSource> timers)
      : _task(std::move(task)), _delay_ms(delay_ms), _timers(std::move(timers)), _has_executed(false) {}

  // Actually starts the timer and schedules the callback
  void startTimer() {
    // Capture shared_ptr to ourselves so we live through the async callback
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
