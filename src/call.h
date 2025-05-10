//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "headers/header.h"
#include "media_stream.h"
#include "rtp/rtp_relay_set.h"
#include "types/sip_identity.h"

using namespace athenasip::types;
using namespace athenasip::headers;

namespace athenasip {

class Channel;

class Call {
 public:
  using SessionsFn = std::function<void(std::shared_ptr<Channel> session)>;

  enum State { Initial, Trying, Ringing, Connected, Closing };

  Call(std::string callId) : id(callId) {}

  std::string id;

  std::shared_ptr<SIPIdentity> from;
  std::shared_ptr<SIPIdentity> to;

  std::unordered_map<int64_t, std::shared_ptr<MediaStream>> streams;

  void add_channel(std::shared_ptr<Channel> session) { _channels[session] = true; }

  bool contains_channel(std::shared_ptr<Channel> session) { return _channels.find(session) != _channels.end(); }

  void with_all_channels(SessionsFn callback) {
    for (const auto& pair : _channels) callback(pair.first);
  }

  void stop_streams() {
    for (const auto& pair : streams) {
      pair.second->stop();
    }
  }

  State state = State::Initial;

 private:
  std::unordered_map<std::shared_ptr<Channel>, bool> _channels;
};

}  // namespace athenasip
