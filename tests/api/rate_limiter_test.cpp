//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/rate_limiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace athenasip;
using namespace std::chrono_literals;

namespace {

const api::RateLimiter::Policy kFivePerMinute{5, 5};

}  // namespace

// A burst is let through at once, and the request after it is not.
TEST(RateLimiterTest, TheBurstIsAllowedAndTheNextIsNot) {
  api::RateLimiter limiter;
  const auto now = api::RateLimiter::Clock::now();

  for (int i = 0; i < 5; ++i) EXPECT_FALSE(limiter.take("a", kFivePerMinute, now).has_value()) << "request " << i;

  const auto refused = limiter.take("a", kFivePerMinute, now);
  ASSERT_TRUE(refused.has_value());

  // Five a minute is one every twelve seconds, and that is how long the next one is.
  EXPECT_EQ(*refused, 12s);
}

// It refills at the rate, so a caller that waits what it was told is let through.
TEST(RateLimiterTest, WaitingWhatItSaidIsEnough) {
  api::RateLimiter limiter;
  auto now = api::RateLimiter::Clock::now();

  for (int i = 0; i < 5; ++i) limiter.take("a", kFivePerMinute, now);

  const auto wait = limiter.take("a", kFivePerMinute, now);
  ASSERT_TRUE(wait.has_value());

  now += *wait;
  EXPECT_FALSE(limiter.take("a", kFivePerMinute, now).has_value());
  EXPECT_TRUE(limiter.take("a", kFivePerMinute, now).has_value());
}

// And it never refills beyond the burst, however long it was left.
TEST(RateLimiterTest, AnIdleKeyRefillsOnlyToTheBurst) {
  api::RateLimiter limiter;
  auto now = api::RateLimiter::Clock::now();

  limiter.take("a", kFivePerMinute, now);
  now += 24h;

  for (int i = 0; i < 5; ++i) EXPECT_FALSE(limiter.take("a", kFivePerMinute, now).has_value());
  EXPECT_TRUE(limiter.take("a", kFivePerMinute, now).has_value());
}

// One caller exhausting its bucket costs nobody else anything.
TEST(RateLimiterTest, KeysAreIndependent) {
  api::RateLimiter limiter;
  const auto now = api::RateLimiter::Clock::now();

  for (int i = 0; i < 6; ++i) limiter.take("a", kFivePerMinute, now);

  EXPECT_FALSE(limiter.take("b", kFivePerMinute, now).has_value());
}

TEST(RateLimiterTest, ZeroIsNoLimit) {
  api::RateLimiter limiter;
  const auto now = api::RateLimiter::Clock::now();

  for (int i = 0; i < 1000; ++i) ASSERT_FALSE(limiter.take("a", {0, 5}, now).has_value());
  EXPECT_EQ(limiter.size(), 0u);
}

// Keys are chosen by callers, so the map is bounded: buckets that have refilled are
// forgotten once it grows past the bound, and the ones still limiting are kept.
TEST(RateLimiterTest, RefilledKeysAreForgottenWhenThereAreTooMany) {
  api::RateLimiter limiter(10);
  auto now = api::RateLimiter::Clock::now();

  for (int i = 0; i < 6; ++i) limiter.take("busy", kFivePerMinute, now);
  for (int i = 0; i < 20; ++i) limiter.take("caller-" + std::to_string(i), kFivePerMinute, now);

  now += 1h;
  for (int i = 0; i < 5; ++i) limiter.take("busy", kFivePerMinute, now);
  limiter.take("one-more", kFivePerMinute, now);

  EXPECT_LE(limiter.size(), 3u);
  EXPECT_TRUE(limiter.take("busy", kFivePerMinute, now).has_value()) << "a bucket still limiting has to survive the prune";
}
