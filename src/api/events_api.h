//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "../events/event_system.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../plugins/plugin.h"
#include "router.h"

namespace athenasip::api {

// GET /api/v1/events: the event bus as Server-Sent Events, for a console that wants to watch rather than poll.
// Each message on nodes/#, subscribers/# and calls/# is one event named by its topic, with the message as its
// data, for as long as the client stays.
class EventsAPI : public std::enable_shared_from_this<EventsAPI> {
 public:
  EventsAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<events::EventSystem> events, plugins::Executor executor, std::size_t limit = 32);

  void register_routes(Router& router);

  // One event in the text/event-stream format: a data line per line of the message.
  static std::string format(const std::string& topic, const std::string& message);

  std::size_t open() const { return _open; }

 private:
  void _stream(RouteContext context);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<events::EventSystem> _events;
  plugins::Executor _executor;
  std::size_t _limit;
  std::atomic<std::size_t> _open{0};
};

}  // namespace athenasip::api
