//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push_refresher.h"

#include <chrono>
#include <utility>

#include "core.h"
#include "push/push_parameters.h"

namespace athenasip {

PushRefresher::PushRefresher(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("push", std::move(logger))), _core(std::move(core)) {}

std::string PushRefresher::_key(std::uint64_t subscriber_id, const types::SIPUri& contact) { return std::to_string(subscriber_id) + " " + contact.to_string(); }

void PushRefresher::watch(std::uint64_t subscriber_id, const std::shared_ptr<types::SIPUri>& contact, std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core || !contact) return;

  const auto key = _key(subscriber_id, *contact);
  forget(subscriber_id, contact);

  // The registrar refuses a push binding shorter than this, so it is a guard rather than a case.
  const auto refresh = core->config->push_refresh;
  if (expires_seconds <= refresh) return;

  _watched[key] = Watched{subscriber_id, contact, std::time(nullptr) + static_cast<std::time_t>(expires_seconds), nullptr};
  _schedule(key, expires_seconds - refresh);
}

void PushRefresher::forget(std::uint64_t subscriber_id, const std::shared_ptr<types::SIPUri>& contact) {
  if (!contact) return;

  auto found = _watched.find(_key(subscriber_id, *contact));
  if (found == _watched.end()) return;

  if (found->second.timer) found->second.timer->cancel();
  _watched.erase(found);
}

void PushRefresher::_schedule(const std::string& key, std::uint32_t seconds) {
  auto core = _core.lock();
  auto found = _watched.find(key);
  if (!core || found == _watched.end()) return;

  auto timers = core->timer_source();
  if (!timers) return;

  std::weak_ptr<PushRefresher> weak_self = weak_from_this();
  auto strand = core->strand();

  found->second.timer = timers->schedule(std::chrono::seconds(seconds), [weak_self, strand, key]() {
    boost::asio::post(strand, [weak_self, key]() {
      if (auto self = weak_self.lock()) self->_due(key);
    });
  });
}

void PushRefresher::_due(const std::string& key) {
  auto core = _core.lock();
  auto found = _watched.find(key);
  if (!core || found == _watched.end()) return;

  const auto watched = found->second;
  auto self = shared_from_this();

  // The store is the truth: another node may have taken the refresh, or the binding may have gone.
  core->location_list(watched.subscriber_id, [this, self, key, watched](plugins::Result<std::vector<types::Location>> found) {
    auto core = _core.lock();
    if (!core || _watched.find(key) == _watched.end()) return;

    if (!found.ok) {
      _logger->warn("Cannot read the binding for " + watched.contact->to_string() + " to refresh it - " + found.error);
      _watched.erase(key);
      return;
    }

    const types::Location* binding = nullptr;
    for (const auto& location : found.value) {
      if (location.contact && location.contact->to_string() == watched.contact->to_string()) binding = &location;
    }

    // Expired, removed, or registered again without push: no further pushes (5.5).
    if (binding == nullptr || !binding->push) {
      _watched.erase(key);
      return;
    }

    // Refreshed since it was watched, perhaps through another node: wait for the new expiry instead.
    if (binding->expires_at > watched.expires_at) {
      const auto later = static_cast<std::uint32_t>(binding->expires_at - watched.expires_at);
      _watched[key].expires_at = binding->expires_at;
      return _schedule(key, later);
    }

    auto notification = push::notification_of(*binding->contact);
    auto service = notification ? core->push_service(notification->provider) : nullptr;
    _watched.erase(key);
    if (!service || !service->refreshes(*notification)) return;

    notification->reason = push::Notification::Reason::Refresh;
    _logger->info("Asking " + watched.contact->to_string() + " to refresh its binding");

    service->send(core->strand(), *notification, [this, self, watched](plugins::Status sent) {
      if (!sent.ok) _logger->warn("The " + watched.contact->parameter("pn-provider") + " push to refresh a binding failed - " + sent.error);
    });
  });
}

}  // namespace athenasip
