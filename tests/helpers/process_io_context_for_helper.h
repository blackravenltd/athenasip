//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <gtest/gtest.h>
#include <chrono>

#include "global_io_context.h"

inline void process_io_context_for(std::chrono::milliseconds duration) {
  auto& io_context = athenasip::detail::get_global_io_context();
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < duration) {
    io_context.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}