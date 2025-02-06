//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "logger.h"

namespace athenasip {

class Server {
 public:
  virtual void start() = 0;
  virtual void stop() = 0;
};

}  // namespace athenasip
