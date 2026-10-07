//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

std::size_t cancels_to_callee(ProxyFixture& f) { return ProxyFixture::requests_with(f.callee_connection, "CANCEL").size(); }

void advance(ProxyFixture& f, std::chrono::seconds by) {
  f.on_strand([&f, by]() { f.timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
}

// An INVITE forwarded to Bob, who rings and then says nothing more.
void ring_for_ever(ProxyFixture& f) {
  f.bind_bob();

  f.receive(f.caller, f.invite());
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  ASSERT_NE(f.response_with(f.caller_connection, 180), nullptr);
}

}  // namespace

// RFC 3261 16.6 step 11: timer C, longer than three minutes, bounds a proxied INVITE.
// Timer B stops at the first provisional (17.1.1.2), so only timer C ends a branch that
// rings and goes quiet.
TEST(ProxyTimerCTest, ABranchThatRingsForEverIsCancelled) {
  ProxyFixture f;
  ring_for_ever(f);

  // Three minutes is the RFC's floor; this node's timer is longer.
  advance(f, std::chrono::seconds(180));
  EXPECT_EQ(cancels_to_callee(f), 0u);

  advance(f, std::chrono::seconds(61));

  // RFC 3261 16.8: a branch that has answered provisionally is sent a CANCEL.
  ASSERT_EQ(cancels_to_callee(f), 1u);

  auto cancel = ProxyFixture::requests_with(f.callee_connection, "CANCEL").front();
  EXPECT_EQ(cancel->header->request_method, "CANCEL");
}

// RFC 3261 16.7 step 2: a provisional response from 101 to 199 resets timer C.
TEST(ProxyTimerCTest, AProvisionalResponseResetsTimerC) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(200));
  ASSERT_EQ(cancels_to_callee(f), 0u);

  f.receive(f.callee, f.response_from_callee(183, "Session Progress"));

  // 400 seconds in total, past the timer had it not been reset.
  advance(f, std::chrono::seconds(200));
  EXPECT_EQ(cancels_to_callee(f), 0u);

  advance(f, std::chrono::seconds(61));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// RFC 3261 16.7 step 2: a 100 Trying does not reset timer C, so a hop cannot hold the
// branch open by repeating it.
TEST(ProxyTimerCTest, A100TryingDoesNotResetTimerC) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(200));
  ASSERT_EQ(cancels_to_callee(f), 0u);

  f.receive(f.callee, f.response_from_callee(100, "Trying"));

  // Still measured from the 180, so 61 more seconds is past the timer.
  advance(f, std::chrono::seconds(61));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// RFC 3261 16.8: a branch that ignores its CANCEL is terminated.
TEST(ProxyTimerCTest, ABranchThatIgnoresItsCancelIsGivenUpOn) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(241));
  ASSERT_EQ(cancels_to_callee(f), 1u);

  // The branch has been asked to stop; the caller is still waiting.
  EXPECT_EQ(f.response_with(f.caller_connection, 408), nullptr);

  advance(f, std::chrono::seconds(241));

  // RFC 3261 16.8: the branch is treated as though it answered 408. With one binding, that
  // is what the caller gets.
  EXPECT_NE(f.response_with(f.caller_connection, 408), nullptr);

  // The transaction is gone, so no further CANCEL is sent.
  advance(f, std::chrono::seconds(241));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// An answered branch stops timer C, so a connected call is not cancelled.
TEST(ProxyTimerCTest, AnAnsweredBranchIsNeverCancelled) {
  ProxyFixture f;
  ring_for_ever(f);

  f.receive(f.callee, f.response_from_callee(200, "OK"));
  ASSERT_NE(f.response_with(f.caller_connection, 200), nullptr);

  advance(f, std::chrono::seconds(600));

  EXPECT_EQ(cancels_to_callee(f), 0u);
}
