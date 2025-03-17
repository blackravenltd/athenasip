//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "sdp.h"
#include "rtp/rtp_relay.h"

namespace athenasip {

class Session;

class MediaStream {
 public:
  std::string id;

  std::shared_ptr<Media> sdp_media;

  std::shared_ptr<rtp::RTPRelaySet> rtp_set;
  std::shared_ptr<rtp::RTPRelaySet> rtcp_set;

  void stop() {
    if(rtp_set) rtp_set->stop();
    if(rtcp_set) rtcp_set->stop();
  }
 private:
};

}  // namespace athenasip
