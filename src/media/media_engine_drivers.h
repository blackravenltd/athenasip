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
#include "rtpengine_media_engine.h"

namespace athenasip::media {

// The in-tree engines: builtin needs no configuration, rtpengine is for production.
inline void register_builtin_media_engines(std::shared_ptr<loggers::Logger> logger) {
  MediaEngine::register_driver<BuiltinMediaEngine>(logger, "builtin");
  MediaEngine::register_driver<RtpengineMediaEngine>(logger, "rtpengine");
}

}  // namespace athenasip::media
