//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "api/rate_limiter.h"

namespace athenasip {

// Digest guessing over SIP (docs/authentication.md, Guessing). Failures are counted by source address and by
// address of record; an address past its allowance is banned, and an AOR past its allowance is closed to every
// address that has not lately authenticated as it, so a guess spread over many addresses stops without locking
// the subscriber's own phone out. Challenges to one address are limited too. Held in memory, per node, like the
// HTTP limits: a guesser spread over the nodes of a cluster gets each node's allowance.
class AuthGuard {
 public:
  using Clock = std::chrono::steady_clock;
  using Policy = api::RateLimiter::Policy;

  struct Limits {
    Policy failures_source;
    Policy failures_aor;
    Policy challenges_source;
    std::chrono::seconds ban{0};
  };

  // `bound` is the most entries each table holds before the oldest go.
  explicit AuthGuard(Limits limits, std::size_t bound = 10000);

  // The time left on the address's ban, or nothing.
  std::optional<std::chrono::seconds> banned(const std::string& source, Clock::time_point now = Clock::now());

  // Whether the AOR is closed to the address: guessed at past its allowance, and not authenticated from there lately.
  std::optional<std::chrono::seconds> aor_closed(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  // Takes one challenge from the address's allowance.
  bool may_challenge(const std::string& source, Clock::time_point now = Clock::now());

  // A wrong answer, or one for no subscriber. True when this one banned the address.
  bool failed(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  // A right answer: the address may go on reaching the AOR while it is closed to others.
  void succeeded(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  std::size_t size();

 private:
  // What `until` holds past `now`, or nothing.
  static std::optional<std::chrono::seconds> _left(std::unordered_map<std::string, Clock::time_point>& table, const std::string& key,
                                                   Clock::time_point now);
  void _bound(std::unordered_map<std::string, Clock::time_point>& table, Clock::time_point now);

  Limits _limits;
  std::size_t _bound_at;

  api::RateLimiter _source_failures;
  api::RateLimiter _aor_failures;
  api::RateLimiter _challenges;

  std::mutex _mutex;
  std::unordered_map<std::string, Clock::time_point> _banned;
  std::unordered_map<std::string, Clock::time_point> _closed;
  // aor + " " + source, until when the address is known to be the AOR's own.
  std::unordered_map<std::string, Clock::time_point> _trusted;
};

}  // namespace athenasip
