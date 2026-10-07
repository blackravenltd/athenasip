//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "../loggers/logger.h"
#include "apns_push_service.h"
#include "fcm_push_service.h"
#include "push_service.h"
#include "webpush_push_service.h"

namespace athenasip::push {

// The providers in the tree, each registered under its RFC 8599 pn-provider value.
inline void register_builtin_push_services(std::shared_ptr<loggers::Logger> logger) {
  PushService::register_driver<WebpushPushService>(logger, "webpush");
  PushService::register_driver<FcmPushService>(logger, "fcm");
  PushService::register_driver<ApnsPushService>(logger, "apns");
}

}  // namespace athenasip::push
