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

AuthGuard::AuthGuard(Limits limits, std::string node, std::size_t bound)
    : _limits(limits), _node(std::move(node)), _bound_at(bound), _source_failures(bound), _aor_failures(bound), _challenges(bound) {}

std::optional<std::chrono::seconds> AuthGuard::_left(Table& table, const std::string& key, Clock::time_point now) {
  const auto found = table.find(key);
  if (found == table.end()) return std::nullopt;
  if (found->second.until <= now) {
    table.erase(found);
    return std::nullopt;
  }
  return std::chrono::ceil<std::chrono::seconds>(found->second.until - now);
}

void AuthGuard::_bound(Table& table, Clock::time_point now) {
  if (table.size() < _bound_at) return;
  for (auto it = table.begin(); it != table.end();) it = it->second.until <= now ? table.erase(it) : std::next(it);
  // Still full: the soonest to end go first.
  while (table.size() >= _bound_at) {
    table.erase(std::min_element(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.second.until < b.second.until; }));
  }
}

void AuthGuard::_hold(Table& table, const std::string& key, std::chrono::seconds left, const std::string& node, Clock::time_point now) {
  if (left.count() <= 0 || _left(table, key, now)) return;
  _bound(table, now);
  table[key] = Entry{now + left, node, std::chrono::system_clock::now()};
}

std::vector<AuthGuard::Held> AuthGuard::_list(Table& table, Clock::time_point now) {
  std::vector<Held> out;
  const auto wall = std::chrono::system_clock::now();
  for (auto it = table.begin(); it != table.end();) {
    if (it->second.until <= now) {
      it = table.erase(it);
      continue;
    }
    const auto left = std::chrono::duration_cast<std::chrono::system_clock::duration>(it->second.until - now);
    out.push_back(Held{it->first, it->second.node, it->second.since, wall + left});
    ++it;
  }
  std::sort(out.begin(), out.end(), [](const Held& a, const Held& b) { return a.until < b.until; });
  return out;
}

std::optional<std::chrono::seconds> AuthGuard::banned(const std::string& source, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _left(_banned, source, now);
}

std::optional<std::chrono::seconds> AuthGuard::aor_closed(const std::string& source, const std::string& aor, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  const auto left = _left(_closed, aor, now);
  if (!left) return std::nullopt;
  if (const auto trusted = _trusted.find(aor + " " + source); trusted != _trusted.end() && trusted->second > now) return std::nullopt;
  return left;
}

bool AuthGuard::may_challenge(const std::string& source, Clock::time_point now) {
  return !_challenges.take(source, _limits.challenges_source, now).has_value();
}

AuthGuard::Failed AuthGuard::failed(const std::string& source, const std::string& aor, Clock::time_point now) {
  const bool source_over = _source_failures.take(source, _limits.failures_source, now).has_value();
  const bool aor_over = _aor_failures.take(aor, _limits.failures_aor, now).has_value();

  Failed out;
  std::lock_guard<std::mutex> lock(_mutex);
  if (_limits.ban.count() == 0) return out;
  if (aor_over && !_left(_closed, aor, now)) {
    _hold(_closed, aor, _limits.ban, _node, now);
    out.closed = true;
  }
  if (source_over && !_left(_banned, source, now)) {
    _hold(_banned, source, _limits.ban, _node, now);
    out.banned = true;
  }
  return out;
}

void AuthGuard::succeeded(const std::string& source, const std::string& aor, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_trusted.size() >= _bound_at) {
    for (auto it = _trusted.begin(); it != _trusted.end();) it = it->second <= now ? _trusted.erase(it) : std::next(it);
    while (_trusted.size() >= _bound_at) _trusted.erase(_trusted.begin());
  }
  _trusted[aor + " " + source] = now + kTrusted;
}

void AuthGuard::ban(const std::string& source, std::chrono::seconds left, const std::string& node, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  _hold(_banned, source, left, node, now);
}

void AuthGuard::close(const std::string& aor, std::chrono::seconds left, const std::string& node, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  _hold(_closed, aor, left, node, now);
}

std::vector<AuthGuard::Held> AuthGuard::bans(Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _list(_banned, now);
}

std::vector<AuthGuard::Held> AuthGuard::closures(Clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _list(_closed, now);
}

bool AuthGuard::lift(const std::string& source) {
  _source_failures.forget(source);
  std::lock_guard<std::mutex> lock(_mutex);
  return _banned.erase(source) > 0;
}

bool AuthGuard::reopen(const std::string& aor) {
  _aor_failures.forget(aor);
  std::lock_guard<std::mutex> lock(_mutex);
  return _closed.erase(aor) > 0;
}

std::size_t AuthGuard::size() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _banned.size() + _closed.size() + _trusted.size() + _source_failures.size() + _aor_failures.size() + _challenges.size();
}

}  // namespace athenasip
