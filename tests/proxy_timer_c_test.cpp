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

// An INVITE forwarded to Bob, who starts ringing and then says nothing more.
void ring_for_ever(ProxyFixture& f) {
  f.bind_bob();

  f.receive(f.caller, f.invite());
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  ASSERT_NE(f.response_with(f.caller_connection, 180), nullptr);
}

}  // namespace

// RFC 3261 16.6 step 11: "Timer C MUST be set for each client transaction when an INVITE
// request is proxied. The timer MUST be larger than 3 minutes."
//
// Nothing else bounds this branch. Timer B gives up on a branch that never answers at
// all, but the 180 moved the client transaction to Proceeding and cancelled it
// (17.1.1.2), so a callee that rings and then goes quiet held the caller, the response
// context and the dialog open for as long as the node ran.
TEST(ProxyTimerCTest, ABranchThatRingsForEverIsCancelled) {
  ProxyFixture f;
  ring_for_ever(f);

  // Three minutes is the floor the RFC sets, and this node's timer is longer than it.
  advance(f, std::chrono::seconds(180));
  EXPECT_EQ(cancels_to_callee(f), 0u);

  advance(f, std::chrono::seconds(61));

  // 16.8: "If the client transaction has received a provisional response, the proxy MUST
  // generate a CANCEL request matching that transaction."
  ASSERT_EQ(cancels_to_callee(f), 1u);

  auto cancel = ProxyFixture::requests_with(f.callee_connection, "CANCEL").front();
  EXPECT_EQ(cancel->header->request_method, "CANCEL");
}

// 16.7 step 2: "if the response is a provisional response with status codes 101 to 199
// inclusive, the proxy MUST reset timer C for that client transaction". A callee that
// keeps saying something is a callee this node keeps waiting for.
TEST(ProxyTimerCTest, AProvisionalResponseResetsTimerC) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(200));
  ASSERT_EQ(cancels_to_callee(f), 0u);

  f.receive(f.callee, f.response_from_callee(183, "Session Progress"));

  // Another 200 seconds, which without the reset would be 400 in total and well past
  // the timer.
  advance(f, std::chrono::seconds(200));
  EXPECT_EQ(cancels_to_callee(f), 0u);

  advance(f, std::chrono::seconds(61));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// The same step excludes 100 by name: "anything but 100". A 100 Trying says the next hop
// received the INVITE, not that anybody is ringing, so a hop that answers 100 and then
// stalls must not be able to hold the branch open by repeating it.
TEST(ProxyTimerCTest, A100TryingDoesNotResetTimerC) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(200));
  ASSERT_EQ(cancels_to_callee(f), 0u);

  f.receive(f.callee, f.response_from_callee(100, "Trying"));

  // Still measured from the 180, so 61 more seconds is past the timer rather than the
  // start of a fresh interval.
  advance(f, std::chrono::seconds(61));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// 16.8 offers a choice: "the proxy MUST either reset the timer with any value it
// chooses, or terminate the client transaction". The CANCEL is the first of those, and
// a far end that ignores it gets the second, or the branch would outlive the node's
// interest in it exactly as it did before timer C existed.
TEST(ProxyTimerCTest, ABranchThatIgnoresItsCancelIsGivenUpOn) {
  ProxyFixture f;
  ring_for_ever(f);

  advance(f, std::chrono::seconds(241));
  ASSERT_EQ(cancels_to_callee(f), 1u);

  // The caller is still waiting: the branch was asked to stop, not declared dead.
  EXPECT_EQ(f.response_with(f.caller_connection, 408), nullptr);

  advance(f, std::chrono::seconds(241));

  // Nothing came back, so the branch is treated the way 16.8 treats one that never
  // answered: as though a 408 had arrived. There is one binding, so that is what the
  // caller gets.
  EXPECT_NE(f.response_with(f.caller_connection, 408), nullptr);

  // And it is not cancelled a third time. The transaction is gone.
  advance(f, std::chrono::seconds(241));
  EXPECT_EQ(cancels_to_callee(f), 1u);
}

// A branch that answers ends the timer with it. Without this a call that connected would
// be cancelled four minutes in.
TEST(ProxyTimerCTest, AnAnsweredBranchIsNeverCancelled) {
  ProxyFixture f;
  ring_for_ever(f);

  f.receive(f.callee, f.response_from_callee(200, "OK"));
  ASSERT_NE(f.response_with(f.caller_connection, 200), nullptr);

  advance(f, std::chrono::seconds(600));

  EXPECT_EQ(cancels_to_callee(f), 0u);
}
