//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "logger.h"
#include "logger_scoped.h"
#include "registrar.h"
#include "sip_message.h"
#include "tls_session.h"

namespace athenasip {

class SIPCore {
 public:
  SIPCore(std::shared_ptr<Logger> logger, std::shared_ptr<Registrar> registrar);

  void process_message(std::shared_ptr<TLSSession> session, std::shared_ptr<SIPMessage> message);

 private:
  std::unique_ptr<Logger> _logger;
  std::shared_ptr<Registrar> _registrar;
};

}  // namespace athenasip
