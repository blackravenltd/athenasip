//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "events/event_system.h"
#include "loggers/logger.h"
#include "media_stream.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "script/script_engine.h"
#include "servers/server.h"
#include "sip_message.h"
#include "transaction.h"
#include "version.h"

namespace athenasip {

class SIPCore : public std::enable_shared_from_this<SIPCore> {
 public:
  SIPCore();
  SIPCore(std::shared_ptr<Logger> logger, std::shared_ptr<Version> version, std::shared_ptr<Config> config, std::shared_ptr<Registrar> _registrar);

  std::shared_ptr<Registrar> registrar;

  void process_message(std::shared_ptr<SIPMessage> message);

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Version> _version;
  std::shared_ptr<Config> _config;

  void _send(std::shared_ptr<SIPMessage> message, uint16_t code, std::string response_message);
};

}  // namespace athenasip
