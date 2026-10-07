//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "global_io_context.h"

// Polls the global io_context until the flag is set or the timeout expires.
inline bool waitForCondition(const std::atomic<bool>& flag, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!flag.load() && (std::chrono::steady_clock::now() - start < timeout)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        athenasip::detail::get_global_io_context().poll();
    }
    return flag.load();
}