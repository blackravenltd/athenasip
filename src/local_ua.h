//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "sip_message.h"
#include "types/dialog.h"

namespace athenasip {

class Core;

// The node as a user agent of its own (RFC 3261 8.1), the fourth transaction user. It ends a call towards both ends:
// each end is sent the BYE its peer would send (12.2.1.1, 15.1.1), entering the proxy as if that peer had sent it,
// so this node's Record-Route and flow tokens carry it to the end as they carry any in-dialog request, and dialog
// tracking ends the dialog, releases the media and closes the record as it does for any BYE.
//
// For this node's own policies and an administrator's request. Never for an RFC 4028 lapse, after which a proxy
// "MUST NOT send a BYE" (8.3). Runs on Core's strand.
class LocalUA : public std::enable_shared_from_this<LocalUA> {
 public:
  LocalUA(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Sends a BYE to both ends of every confirmed dialog of the call. False when this node holds none.
  bool hang_up(const std::string& call_id, const std::string& reason);

 private:
  // The BYE the far end would send to this one: to the callee as the caller, or to the caller as the callee.
  std::shared_ptr<SIPMessage> _bye_to(const types::Dialog& dialog, bool callee) const;

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
};

}  // namespace athenasip
