//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "auth_guard.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace athenasip;
using namespace std::chrono_literals;

namespace {

AuthGuard::Limits limits() {
  AuthGuard::Limits out;
  out.failures_source = {3, 1};
  out.failures_aor = {5, 1};
  out.challenges_source = {4, 1};
  out.ban = 600s;
  return out;
}

}  // namespace

// Review finding 24: a source that keeps answering challenges wrongly is guessing. Past its allowance it is banned
// for the ban time, and the failure that crossed the line says so.
TEST(AuthGuardTest, ASourceThatFailsPastItsAllowanceIsBanned) {
  AuthGuard guard(limits());
  const auto now = AuthGuard::Clock::now();

  EXPECT_FALSE(guard.banned("198.51.100.7", now));
  EXPECT_FALSE(guard.failed("198.51.100.7", "sip:alice@example.com", now));
  EXPECT_FALSE(guard.failed("198.51.100.7", "sip:bob@example.com", now));
  EXPECT_FALSE(guard.failed("198.51.100.7", "sip:carol@example.com", now));
  EXPECT_FALSE(guard.banned("198.51.100.7", now));

  EXPECT_TRUE(guard.failed("198.51.100.7", "sip:dave@example.com", now)) << "the fourth failure bans";
  ASSERT_TRUE(guard.banned("198.51.100.7", now + 1s));
  EXPECT_EQ(*guard.banned("198.51.100.7", now + 1s), 599s);

  // Another address is not.
  EXPECT_FALSE(guard.banned("198.51.100.8", now));

  // The ban ends.
  EXPECT_FALSE(guard.banned("198.51.100.7", now + 601s));
}

// Guessing one subscriber's password from many addresses: past the AOR's allowance the AOR is closed to every
// address that has not lately authenticated as it, so its own phone goes on registering.
TEST(AuthGuardTest, AnAorGuessedAtFromManyAddressesIsClosedToStrangersOnly) {
  AuthGuard guard(limits());
  const auto now = AuthGuard::Clock::now();

  guard.succeeded("192.0.2.10", "sip:alice@example.com", now);

  for (int i = 0; i < 6; ++i) guard.failed("203.0.113." + std::to_string(i), "sip:alice@example.com", now);

  EXPECT_TRUE(guard.aor_closed("203.0.113.99", "sip:alice@example.com", now));
  EXPECT_FALSE(guard.aor_closed("192.0.2.10", "sip:alice@example.com", now)) << "alice's own phone";
  EXPECT_FALSE(guard.aor_closed("203.0.113.99", "sip:bob@example.com", now)) << "another AOR";
  EXPECT_FALSE(guard.aor_closed("203.0.113.99", "sip:alice@example.com", now + 601s));
}

// Challenges are cheap, but a flood of them from one address is still a flood: past the allowance the address is
// refused rather than challenged.
TEST(AuthGuardTest, ChallengesToOneAddressAreLimited) {
  AuthGuard guard(limits());
  const auto now = AuthGuard::Clock::now();

  for (int i = 0; i < 4; ++i) EXPECT_TRUE(guard.may_challenge("198.51.100.7", now));
  EXPECT_FALSE(guard.may_challenge("198.51.100.7", now));
  EXPECT_TRUE(guard.may_challenge("198.51.100.8", now));
  EXPECT_TRUE(guard.may_challenge("198.51.100.7", now + 61s));
}

// Zero in a limit turns it off, as for the HTTP limits.
TEST(AuthGuardTest, AZeroLimitIsNoLimit) {
  AuthGuard::Limits off;
  AuthGuard guard(off);
  const auto now = AuthGuard::Clock::now();

  for (int i = 0; i < 100; ++i) EXPECT_FALSE(guard.failed("198.51.100.7", "sip:alice@example.com", now));
  EXPECT_FALSE(guard.banned("198.51.100.7", now));
  EXPECT_FALSE(guard.aor_closed("198.51.100.7", "sip:alice@example.com", now));
  EXPECT_TRUE(guard.may_challenge("198.51.100.7", now));
}

// Bounded: made-up AORs and addresses cannot grow it without limit.
TEST(AuthGuardTest, ItIsBounded) {
  AuthGuard guard(limits(), 50);
  const auto now = AuthGuard::Clock::now();

  for (int i = 0; i < 1000; ++i) {
    const auto n = std::to_string(i);
    guard.failed("10.0." + std::to_string(i / 250) + "." + std::to_string(i % 250), "sip:u" + n + "@example.com", now);
    guard.succeeded("10.1.0." + std::to_string(i % 250), "sip:u" + n + "@example.com", now);
  }
  EXPECT_LE(guard.size(), 4u * 50u);
}
