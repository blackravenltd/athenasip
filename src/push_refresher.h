//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "timer_source.h"
#include "types/sip_uri.h"

namespace athenasip {

class Core;

// RFC 8599 5.5: a push binding is woken push.refresh seconds before it expires, so a client that cannot keep
// its own timers refreshes it in time. A binding refreshed meanwhile, here or on another node, is not pushed.
class PushRefresher : public std::enable_shared_from_this<PushRefresher> {
 public:
  PushRefresher(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Starts or restarts the refresh for a binding just registered with push.
  void watch(std::uint64_t subscriber_id, const std::shared_ptr<types::SIPUri>& contact, std::uint32_t expires_seconds);

  // Stops it, for a binding removed or registered again without push (5.5: no further pushes).
  void forget(std::uint64_t subscriber_id, const std::shared_ptr<types::SIPUri>& contact);

  std::size_t size() const { return _watched.size(); }

 private:
  struct Watched {
    std::uint64_t subscriber_id = 0;
    std::shared_ptr<types::SIPUri> contact;
    std::time_t expires_at = 0;
    std::shared_ptr<Timer> timer;
  };

  static std::string _key(std::uint64_t subscriber_id, const types::SIPUri& contact);
  void _schedule(const std::string& key, std::uint32_t seconds);
  void _due(const std::string& key);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  std::map<std::string, Watched> _watched;
};

}  // namespace athenasip
