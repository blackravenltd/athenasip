//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "auth_guard.h"

#include <algorithm>

namespace athenasip {

namespace {
// How long a right answer vouches for its address: longer than a registration lasts, so a phone that registers
// hourly stays known.
constexpr std::chrono::hours kTrusted{2};
}  // namespace

AuthGuard::AuthGuard(Limits limits, std::size_t bound)
    : _limits(limits), _bound_at(bound), _source_failures(bound), _aor_failures(bound), _challenges(bound) {}

std::optional<std::chrono::seconds> AuthGuard::_left(std::unordered_map<std::string, Clock::time_point>& table, const std::string& key,
                                                     Clock::time_point now) {
  const auto found = table.find(key);
  if (found == table.end()) return std::nullopt;
  if (found->second <= now) {
    table.erase(found);
    return std::nullopt;
  }
  return std::chrono::ceil<std::chrono::seconds>(found->second - now);
}

void AuthGuard::_bound(std::unordered_map<std::string, Clock::time_point>& table, Clock::time_point now) {
  if (table.size() < _bound_at) return;
  for (auto it = table.begin(); it != table.end();) it = it->second <= now ? table.erase(it) : std::next(it);
  // Still full: the soonest to end go first.
  while (table.size() >= _bound_at) {
    table.erase(std::min_element(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.second < b.second; }));
  }
}

std::optional<std::chrono::seconds> AuthGuard::banned(const std::string& source, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _left(_banned, source, now);
}

std::optional<std::chrono::seconds> AuthGuard::aor_closed(const std::string& source, const std::string& aor, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  const auto left = _left(_closed, aor, now);
  if (!left) return std::nullopt;
  if (_left(_trusted, aor + " " + source, now)) return std::nullopt;
  return left;
}

bool AuthGuard::may_challenge(const std::string& source, Clock::time_point now) {
  return !_challenges.take(source, _limits.challenges_source, now).has_value();
}

bool AuthGuard::failed(const std::string& source, const std::string& aor, Clock::time_point now) {
  const bool source_over = _source_failures.take(source, _limits.failures_source, now).has_value();
  const bool aor_over = _aor_failures.take(aor, _limits.failures_aor, now).has_value();

  std::lock_guard<std::mutex> lock(_mutex);
  if (aor_over && _limits.ban.count() > 0 && !_left(_closed, aor, now)) {
    _bound(_closed, now);
    _closed[aor] = now + _limits.ban;
  }
  if (!source_over || _limits.ban.count() == 0 || _left(_banned, source, now)) return false;
  _bound(_banned, now);
  _banned[source] = now + _limits.ban;
  return true;
}

void AuthGuard::succeeded(const std::string& source, const std::string& aor, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  _bound(_trusted, now);
  _trusted[aor + " " + source] = now + kTrusted;
}

std::size_t AuthGuard::size() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _banned.size() + _closed.size() + _trusted.size() + _source_failures.size() + _aor_failures.size() + _challenges.size();
}

}  // namespace athenasip
