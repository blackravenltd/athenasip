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
  // The io_context for the entire process
  static boost::asio::io_context io_context;

  // The static work guard so the io_context.run() never stops by itself
  static auto work_guard = boost::asio::make_work_guard(io_context);

  // A single static thread calling io_context.run()
  // Created once, destroyed on program exit
  static struct RunThread {
    RunThread() {
      t = std::thread([&]() { io_context.run(); });
    }
    ~RunThread() {
      // Release the work guard so run() eventually stops
      work_guard.reset();

      // Releasing the guard is not enough: a pending timer is work in its own right, and
      // a registration expiry an hour out would hold run() open for an hour. By the time
      // this runs the process is leaving, so abandon them rather than wait.
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
