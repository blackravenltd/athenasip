//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "../loggers/logger.h"
#include "builtin_policy.h"
#include "lua_policy.h"
#include "policy.h"

namespace athenasip::policy {

inline void register_builtin_policies(std::shared_ptr<loggers::Logger> logger) {
  Policy::register_driver<BuiltinPolicy>(logger, "builtin");
  Policy::register_driver<LuaPolicy>(logger, "lua");
}

}  // namespace athenasip::policy
