//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "databases/db.h"
#include "loggers/logger.h"
#include "media_stream.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "servers/server.h"
#include "sip_message.h"
#include "version.h"

namespace athenasip {

class SIPCore {
 public:
  SIPCore();
  SIPCore(std::shared_ptr<Logger> _logger, std::shared_ptr<Version> _version, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::databases::DB> _db,
          std::shared_ptr<Registrar> _registrar);

 public:
  std::shared_ptr<Logger> logger;
  std::shared_ptr<Version> version;
  std::shared_ptr<Config> config;
  std::shared_ptr<databases::DB> db;
  std::shared_ptr<rtp::RTPRelay> rtprelay;
  std::shared_ptr<Registrar> registrar;
  std::vector<std::shared_ptr<servers::Server>> servers;

  void process_message(std::shared_ptr<SIPMessage> message);
  void _process_call_state(std::shared_ptr<SIPMessage> message, std::shared_ptr<Call> call);
  void _send_auth_challenge(std::shared_ptr<SIPMessage> message);
  void _process_message_register(std::shared_ptr<SIPMessage> message);
  void _process_message_publish(std::shared_ptr<SIPMessage> message);
  void _process_message_invite(std::shared_ptr<SIPMessage> message);
  void _map_media(std::shared_ptr<Call> call, std::shared_ptr<SDP> sdp);
  void _rewrite_sdp(std::shared_ptr<SDP> sdp, std::string server_address, uint16_t rtp_port, uint16_t rtcp_port);
  void _send(std::shared_ptr<SIPMessage> message, uint16_t code, std::string response_message);

  std::string _generate_nonce() const;
};

}  // namespace athenasip
