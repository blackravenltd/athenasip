//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "headers/header.h"
#include "media_stream.h"
#include "types/sip_identity.h"
#include "types/sip_uri.h"

using namespace athenasip::types;
using namespace athenasip::headers;

namespace athenasip {

class Channel;

// A call is multi-party from the start. A two-party call is the case where there are
// two participants and no focus; a conference is the case where there are more and a
// focus URI (RFC 4579). Nothing here assumes there are exactly two legs.
class Call {
 public:
  using ChannelsFn = std::function<void(std::shared_ptr<Channel> channel)>;

  enum State { Initial, Trying, Ringing, Connected, Closing, Closed };

  static std::string state_to_string(const State s) {
    switch (s) {
      case State::Initial:
        return "Initial";
      case State::Trying:
        return "Trying";
      case State::Ringing:
        return "Ringing";
      case State::Connected:
        return "Connected";
      case State::Closing:
        return "Closing";
      case State::Closed:
        return "Closed";
    }
    return "Unknown";
  }

  // One leg of the call. The channel is held weakly: a participant outlives the
  // connection it arrived on, and in a cluster the leg may not be on this node at all,
  // in which case node_id says whose it is and channel is empty.
  struct Participant {
    std::shared_ptr<SIPIdentity> identity;
    std::weak_ptr<Channel> channel;
    std::string node_id;

    // Dialog identifiers (RFC 3261 section 12).
    std::string local_tag;
    std::string remote_tag;

    // Media for this leg, keyed by the SDP media identifier.
    std::unordered_map<int64_t, std::shared_ptr<MediaStream>> streams;

    // The party that started the call.
    bool originator = false;

    void stop_streams() {
      for (const auto& [id, stream] : streams) {
        if (stream) stream->stop();
      }
    }
  };

  std::string id;
  State state = State::Initial;

  // Set when the call is a conference and this node is routing to a focus (RFC 4579).
  // Empty for an ordinary two-party call.
  std::shared_ptr<SIPUri> focus;

  std::vector<Participant> participants;

  std::time_t created_at = 0;
  std::time_t answered_at = 0;
  std::time_t ended_at = 0;

  Participant& add_participant(std::shared_ptr<SIPIdentity> identity, std::shared_ptr<Channel> channel = nullptr, bool originator = false) {
    Participant participant;
    participant.identity = std::move(identity);
    participant.channel = channel;
    participant.originator = originator;

    participants.push_back(std::move(participant));
    return participants.back();
  }

  // The party that started the call, if one is marked.
  const Participant* originator() const {
    for (const auto& participant : participants) {
      if (participant.originator) return &participant;
    }
    return nullptr;
  }

  // Everyone who is not the originator. For a two-party call this is the one callee.
  std::vector<const Participant*> targets() const {
    std::vector<const Participant*> result;
    for (const auto& participant : participants) {
      if (!participant.originator) result.push_back(&participant);
    }
    return result;
  }

  bool contains_channel(const std::shared_ptr<Channel>& channel) const {
    for (const auto& participant : participants) {
      if (participant.channel.lock() == channel) return true;
    }
    return false;
  }

  void with_all_channels(ChannelsFn callback) const {
    for (const auto& participant : participants) {
      if (auto channel = participant.channel.lock()) callback(channel);
    }
  }

  void stop_streams() {
    for (auto& participant : participants) participant.stop_streams();
  }
};

}  // namespace athenasip
