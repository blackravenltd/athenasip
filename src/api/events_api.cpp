//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "events_api.h"

#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "../types/user.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

// What a console watches: nodes coming and going, subscribers registering, calls starting and ending.
constexpr const char* kTopics[] = {"nodes/#", "subscribers/#", "calls/#"};

}  // namespace

EventsAPI::EventsAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<events::EventSystem> events, plugins::Executor executor, std::size_t limit)
    : _logger(std::make_shared<loggers::LoggerScoped>("events_api", std::move(logger))),
      _events(std::move(events)),
      _executor(std::move(executor)),
      _limit(limit) {}

void EventsAPI::register_routes(Router& router) {
  auto self = shared_from_this();
  router.add(http::verb::get, "/api/v1/events", {types::roles::view_cluster_status}, [self](RouteContext c) { self->_stream(std::move(c)); });
}

std::string EventsAPI::format(const std::string& topic, const std::string& message) {
  std::string out = "event: " + topic + "\n";

  std::istringstream lines(message);
  std::string line;
  bool any = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out += "data: " + line + "\n";
    any = true;
  }
  if (!any) out += "data: \n";

  return out + "\n";
}

void EventsAPI::_stream(RouteContext context) {
  // Each stream holds a connection and three subscriptions, so there is a ceiling.
  if (_open >= _limit) {
    write_error(context.response, http::status::service_unavailable, "too_many_streams", "this node is already serving its limit of event streams");
    return context.done();
  }

  auto self = shared_from_this();

  // Counted from when the headers are out, so a stream whose headers never went is not counted.
  context.stream([this, self](std::shared_ptr<ResponseStream> out) {
    ++_open;

    // A client that loses the connection asks again after five seconds.
    out->write("retry: 5000\n\n");

    struct Held {
      std::mutex mutex;
      std::vector<std::shared_ptr<events::Subscription>> subscriptions;
      bool closed = false;
    };
    auto held = std::make_shared<Held>();
    std::weak_ptr<ResponseStream> weak_out = out;

    for (const auto* topic : kTopics) {
      _events->subscribe(
          _executor, topic,
          [weak_out](std::string event, std::string message) {
            if (auto stream = weak_out.lock()) stream->write(format(event, message));
          },
          [this, self, held](plugins::Result<std::shared_ptr<events::Subscription>> subscribed) {
            if (!subscribed.ok) return _logger->warn("An event stream could not listen - " + subscribed.error);

            std::unique_lock<std::mutex> lock(held->mutex);
            if (!held->closed) return held->subscriptions.push_back(subscribed.value);
            lock.unlock();
            _events->unsubscribe(_executor, subscribed.value, [](plugins::Status) {});
          });
    }

    out->on_close([this, self, held]() {
      std::vector<std::shared_ptr<events::Subscription>> subscriptions;
      {
        std::lock_guard<std::mutex> lock(held->mutex);
        held->closed = true;
        subscriptions.swap(held->subscriptions);
      }
      for (const auto& subscription : subscriptions) _events->unsubscribe(_executor, subscription, [](plugins::Status) {});
      --_open;
    });
  });

  context.done();
}

}  // namespace athenasip::api
