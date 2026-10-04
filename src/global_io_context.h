//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <thread>

namespace athenasip::detail {

inline boost::asio::io_context& get_global_io_context() {
  // The process-wide io_context, run by one thread for the life of the process.
  static boost::asio::io_context io_context;

  // Keeps run() from returning when idle.
  static auto work_guard = boost::asio::make_work_guard(io_context);

  static struct RunThread {
    RunThread() {
      t = std::thread([&]() { io_context.run(); });
    }
    ~RunThread() {
      work_guard.reset();

      // Pending timers also keep run() going (a registration expiry may be an hour out), so
      // stop the context rather than wait for them.
      io_context.stop();

      if (t.joinable()) {
        t.join();
      }
    }
    std::thread t;
  } runThread;

  return io_context;
}

}  // namespace athenasip::detail
