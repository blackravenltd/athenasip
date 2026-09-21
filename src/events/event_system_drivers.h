//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "../loggers/logger.h"
#include "event_system.h"
#include "local_event_system.h"
#include "mqtt_event_system.h"

namespace athenasip::events {

inline void register_builtin_event_systems(std::shared_ptr<loggers::Logger> logger) {
  EventSystem::register_driver<LocalEventSystem>(logger, "athena");
  EventSystem::register_driver<LocalEventSystem>(logger, "evt+athena");
  EventSystem::register_driver<LocalEventSystem>(logger, "local");
  EventSystem::register_driver<LocalEventSystem>(logger, "memory");

  EventSystem::register_driver<MQTTEventSystem>(logger, "mqtt");
  EventSystem::register_driver<MQTTEventSystem>(logger, "mqtt5");
  EventSystem::register_driver<MQTTEventSystem>(logger, "evt+mqtt");
}

}  // namespace athenasip::events
