//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>

#include "../sip_message.h"

namespace athenasip::script {

class ScriptEngine {
 public:
  ScriptEngine() = default;
  ~ScriptEngine() = default;

  virtual void start() = 0;
  virtual void reload() = 0;
  virtual void stop() = 0;

  // Events In
  virtual void on_message(std::shared_ptr<SIPMessage> msg) = 0;

  // Events out
  virtual void send_message(std::function<void(std::shared_ptr<SIPMessage> msg)>) = 0;
};

}  // namespace athenasip::script