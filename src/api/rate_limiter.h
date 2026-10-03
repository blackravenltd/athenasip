//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace athenasip::api {

// Token buckets keyed by whatever the caller is limited as: a source address, a session, a
// username. A key gets `burst` requests at once and then `per_minute` a minute, and a
// refusal says how long until the next one would be let through, for a Retry-After.
//
// Held in memory, per node. A limit that has to agree across a cluster would be a datastore
// round trip on every request, and a node that is being hammered is the one that needs to
// say no without asking anybody.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  struct Policy {
    std::uint32_t burst = 0;
    std::uint32_t per_minute = 0;

    // Zero in either is no limit, which is what an operator who fronts the API with
    // something that limits already sets.
    bool unlimited() const { return burst == 0 || per_minute == 0; }
  };

  // Beyond this many keys the full buckets are forgotten: a bucket that has refilled says
  // nothing a new one would not. The keys are chosen by callers, so without a bound a
  // stream of made-up usernames would grow this for the life of the process.
  explicit RateLimiter(std::size_t prune_above = 10000) : _prune_above(prune_above) {}

  // Takes one request from `key`'s bucket. Nothing when it was let through; the wait until
  // it would have been, rounded up to a whole second and never zero, when it was not.
  std::optional<std::chrono::seconds> take(const std::string& key, const Policy& policy, Clock::time_point now = Clock::now()) {
    if (policy.unlimited()) return std::nullopt;

    std::lock_guard<std::mutex> lock(_mutex);

    if (_buckets.size() > _prune_above) _prune(now);

    auto [it, made] = _buckets.try_emplace(key, Bucket{static_cast<double>(policy.burst), now, policy});
    auto& bucket = it->second;

    if (!made) _refill(bucket, now);

    if (bucket.tokens >= 1.0) {
      bucket.tokens -= 1.0;
      return std::nullopt;
    }

    const auto seconds_per_token = 60.0 / policy.per_minute;
    const auto wait = std::ceil((1.0 - bucket.tokens) * seconds_per_token);

    return std::chrono::seconds(std::max<std::int64_t>(1, static_cast<std::int64_t>(wait)));
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _buckets.size();
  }

 private:
  struct Bucket {
    double tokens;
    Clock::time_point at;
    Policy policy;
  };

  static void _refill(Bucket& bucket, Clock::time_point now) {
    if (now <= bucket.at) return;

    const auto elapsed = std::chrono::duration<double>(now - bucket.at).count();
    bucket.tokens = std::min<double>(bucket.policy.burst, bucket.tokens + elapsed * bucket.policy.per_minute / 60.0);
    bucket.at = now;
  }

  void _prune(Clock::time_point now) {
    for (auto it = _buckets.begin(); it != _buckets.end();) {
      _refill(it->second, now);
      it = it->second.tokens >= it->second.policy.burst ? _buckets.erase(it) : std::next(it);
    }
  }

  mutable std::mutex _mutex;
  std::size_t _prune_above;
  std::unordered_map<std::string, Bucket> _buckets;
};

// What each kind of caller is allowed, chosen for the callers there are: a person at a
// login prompt, a monitor polling health, the console polling every two seconds.
struct RateLimits {
  // Open routes, and any request whose credential did not resolve, per source address.
  // Tight, because nothing but a source address is known about who is asking.
  RateLimiter::Policy open{30, 30};

  // The login on top of that: per source address, and per username whoever is asking, so
  // spreading guesses over many addresses buys nothing against one user.
  RateLimiter::Policy login_source{10, 5};
  RateLimiter::Policy login_user{5, 1};

  // Per session. Generous, because it is somebody who has signed in, and the console
  // polls: a two-second poll is thirty a minute, and a busy screen makes several.
  RateLimiter::Policy session{60, 300};
};

// The limiter, the limits and the clock, shared by the router and the routes that limit
// further on what only they can see, which is the login and its username.
class Throttle {
 public:
  explicit Throttle(RateLimits limits = {}) : _limits(limits) {}

  const RateLimits& limits() const { return _limits; }
  void limits_set(RateLimits limits) { _limits = limits; }

  // Injectable, so a test advances time rather than waiting a minute for a bucket.
  void clock_set(std::function<RateLimiter::Clock::time_point()> now) { _now = std::move(now); }

  std::optional<std::chrono::seconds> take(const std::string& key, const RateLimiter::Policy& policy) { return _limiter.take(key, policy, _now()); }

 private:
  RateLimits _limits;
  RateLimiter _limiter;
  std::function<RateLimiter::Clock::time_point()> _now = [] { return RateLimiter::Clock::now(); };
};

}  // namespace athenasip::api
