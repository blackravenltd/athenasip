//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "push/push_service.h"

// A push notification service that records what it was asked to send. It needs a pn-prid, and a pn-param when
// `needs_param` is set; `fails` makes every push fail, as a provider refusing the token would.
class FakePushService : public athenasip::push::PushService {
 public:
  explicit FakePushService(std::string provider, std::vector<std::pair<std::string, std::string>> capabilities = {})
      : _provider(std::move(provider)), _capabilities(std::move(capabilities)) {}

  std::string name() const override { return _provider; }
  std::string version() const override { return "0.0.1"; }

  bool accepts(const athenasip::push::Notification& notification) const override {
    return !notification.prid.empty() && (!needs_param || !notification.param.empty());
  }

  bool refreshes(const athenasip::push::Notification&) const override { return refresh_pushes; }

  std::vector<std::pair<std::string, std::string>> capabilities() const override { return _capabilities; }

  void send(athenasip::plugins::Executor on, athenasip::push::Notification notification, athenasip::plugins::StatusHandler handler) override {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _sent.push_back(std::move(notification));
    }
    _complete(std::move(on), std::move(handler),
              fails ? athenasip::plugins::Status::failure("the provider refused the token") : athenasip::plugins::Status::success());
  }

  std::vector<athenasip::push::Notification> sent() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sent;
  }

  bool needs_param = false;
  bool refresh_pushes = true;
  bool fails = false;

 private:
  std::string _provider;
  std::vector<std::pair<std::string, std::string>> _capabilities;

  mutable std::mutex _mutex;
  std::vector<athenasip::push::Notification> _sent;
};
