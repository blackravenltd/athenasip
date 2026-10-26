//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "api/rate_limiter.h"

namespace athenasip {

// Digest guessing over SIP (docs/authentication.md, Guessing). Failures are counted by source address and by
// address of record; an address past its allowance is banned, and an AOR past its allowance is closed to every
// address that has not lately authenticated as it, so a guess spread over many addresses stops without locking
// the subscriber's own phone out. Challenges to one address are limited too. Counted in memory, per node, like the
// HTTP limits; a ban or a closure is told to the other nodes over the bus and held by each (Core), so the list of
// them is the cluster's and a lift reaches every node.
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

  // A ban on an address, or a closure of an AOR, as listed: by whom and from when until when, wall clock.
  struct Held {
    std::string key;
    std::string node;
    std::chrono::system_clock::time_point since;
    std::chrono::system_clock::time_point until;
  };

  // What one failure did, beyond being counted.
  struct Failed {
    bool banned = false;
    bool closed = false;
  };

  // `node` is this node, named on what it bans. `bound` is the most entries each table holds before the oldest go.
  explicit AuthGuard(Limits limits, std::string node = "", std::size_t bound = 10000);

  // The time left on the address's ban, or nothing.
  std::optional<std::chrono::seconds> banned(const std::string& source, Clock::time_point now = Clock::now());

  // Whether the AOR is closed to the address: guessed at past its allowance, and not authenticated from there lately.
  std::optional<std::chrono::seconds> aor_closed(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  // Takes one challenge from the address's allowance.
  bool may_challenge(const std::string& source, Clock::time_point now = Clock::now());

  // A wrong answer, or one for no subscriber.
  Failed failed(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  // A right answer: the address may go on reaching the AOR while it is closed to others.
  void succeeded(const std::string& source, const std::string& aor, Clock::time_point now = Clock::now());

  // A ban or a closure another node made, held here for as long as it has left.
  void ban(const std::string& source, std::chrono::seconds left, const std::string& node, Clock::time_point now = Clock::now());
  void close(const std::string& aor, std::chrono::seconds left, const std::string& node, Clock::time_point now = Clock::now());

  // What is in force, soonest to end first.
  std::vector<Held> bans(Clock::time_point now = Clock::now());
  std::vector<Held> closures(Clock::time_point now = Clock::now());

  // Lifted by an administrator: the ban or the closure goes, and so does what was counted against the address or the
  // AOR, so the next wrong password does not bring it straight back. Whether there was one.
  bool lift(const std::string& source);
  bool reopen(const std::string& aor);

  std::size_t size();

 private:
  struct Entry {
    Clock::time_point until;
    std::string node;
    std::chrono::system_clock::time_point since;
  };
  using Table = std::unordered_map<std::string, Entry>;

  // What is left of the key's entry past `now`, or nothing; an ended one is dropped.
  static std::optional<std::chrono::seconds> _left(Table& table, const std::string& key, Clock::time_point now);
  void _bound(Table& table, Clock::time_point now);
  void _hold(Table& table, const std::string& key, std::chrono::seconds left, const std::string& node, Clock::time_point now);
  std::vector<Held> _list(Table& table, Clock::time_point now);

  Limits _limits;
  std::string _node;
  std::size_t _bound_at;

  api::RateLimiter _source_failures;
  api::RateLimiter _aor_failures;
  api::RateLimiter _challenges;

  std::mutex _mutex;
  Table _banned;
  Table _closed;
  // aor + " " + source, until when the address is known to be the AOR's own.
  std::unordered_map<std::string, Clock::time_point> _trusted;
};

}  // namespace athenasip
