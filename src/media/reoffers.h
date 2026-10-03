//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "media_profile.h"

namespace athenasip::media {

// A subscriber whose endpoint refused the profile this node offered it and was offered the
// other. took is what it then accepted, or empty when it refused that as well.
struct Reoffer {
  std::string subscriber;
  Profile rejected = Profile::PlainRtp;
  std::optional<Profile> took;
  std::uint64_t count = 0;
  std::time_t last_at = 0;
};

// What this node has had to re-offer, kept for the operator rather than acted on: the node
// suggests a subscriber's profile and the operator decides. Nothing here changes how a later
// call is routed. Per node and in memory, like the live calls, and bounded so a run of
// unknown callees cannot grow it without limit.
class Reoffers {
 public:
  static constexpr std::size_t kLimit = 1024;

  void record(const std::string& subscriber, Profile rejected, std::optional<Profile> took, std::time_t now = std::time(nullptr)) {
    auto& entry = _entries[{subscriber, rejected}];
    entry.subscriber = subscriber;
    entry.rejected = rejected;
    entry.took = took;
    entry.count++;
    entry.last_at = now;

    if (_entries.size() <= kLimit) return;

    auto oldest = std::min_element(_entries.begin(), _entries.end(), [](const auto& a, const auto& b) { return a.second.last_at < b.second.last_at; });
    _entries.erase(oldest);
  }

  // Most recent first.
  std::vector<Reoffer> list() const {
    std::vector<Reoffer> out;
    out.reserve(_entries.size());
    for (const auto& [key, entry] : _entries) out.push_back(entry);
    std::stable_sort(out.begin(), out.end(), [](const Reoffer& a, const Reoffer& b) { return a.last_at > b.last_at; });
    return out;
  }

 private:
  std::map<std::pair<std::string, Profile>, Reoffer> _entries;
};

}  // namespace athenasip::media
