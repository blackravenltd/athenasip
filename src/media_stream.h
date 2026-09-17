//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "rtp/rtp_relay.h"
#include "sdp.h"

namespace athenasip {

class Channel;

class MediaStream {
 public:
  std::string id;

  std::weak_ptr<rtp::RTPRelaySet> rtp_set;
  std::weak_ptr<rtp::RTPRelaySet> rtcp_set;

  void stop() {
    if (!rtp_set.expired()) rtp_set.lock()->stop();
    if (!rtcp_set.expired()) rtcp_set.lock()->stop();
  }

 private:
};

}  // namespace athenasip
