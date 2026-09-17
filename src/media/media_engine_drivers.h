//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "../loggers/logger.h"
#include "builtin_media_engine.h"
#include "media_engine.h"

namespace athenasip::media {

// Two implementations, as with the datastores and event systems: one built in for a
// zero-config single node, one canonical for production. rtpengine:// arrives in M3.
void register_builtin_media_engines(std::shared_ptr<loggers::Logger> logger) { MediaEngine::register_driver<BuiltinMediaEngine>(logger, "builtin"); }

}  // namespace athenasip::media
