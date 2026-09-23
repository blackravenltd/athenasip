//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "headers/header.h"
#include "media/media_profile.h"
#include "media_stream.h"
#include "types/dialog.h"
#include "types/realm.h"
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

    // The dialog this leg takes part in (RFC 3261 section 12). Held rather than the two
    // bare tags it used to be, because the tags alone cannot say where the leg's
    // requests go, what route they take or whether the call is still up, and those are
    // the questions a node that anchors media has to answer.
    //
    // A proxied two-party call has one dialog end to end, so both participants point at
    // the same one. A conference has one per participant, each with the focus (RFC
    // 4579), which is why this hangs off the leg and not off the call.
    std::shared_ptr<Dialog> dialog;

    // Media for this leg, keyed by the SDP media identifier.
    std::unordered_map<int64_t, std::shared_ptr<MediaStream>> streams;

    // What this leg has said its media is, from the last description that arrived from
    // it. A leg's own offer or answer says exactly whether it asked for ICE, DTLS or
    // SRTP, and that is a better answer than the transport it signals over: AthenaPhone
    // is WebRTC on UDP, and RFC 7118 allows a plain phone on a WebSocket. Empty until
    // this node has heard the leg describe itself, which is the only case where the
    // realm and the transport have to decide.
    std::optional<media::Profile> profile;

    // The party that started the call.
    bool originator = false;
  };

  std::string id;
  State state = State::Initial;

  // Set when the call is a conference and this node is routing to a focus (RFC 4579).
  // Empty for an ordinary two-party call.
  std::shared_ptr<SIPUri> focus;

  // The realm's media policy, remembered when the call was set up. A re-INVITE travels
  // in-dialog on its route set and never looks a realm up, so reading the policy again
  // for each message would have hold and resume behave differently from the INVITE
  // that started the call. It is decided once, where the realm is already in hand.
  MediaPolicy media_policy;

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

  // The index of the leg that started the call, or of one that did not. The media
  // contract addresses a participant by index, so a caller that knows which end a
  // session description came from has to be able to turn that into one.
  std::optional<std::size_t> participant_index(bool originator_wanted) const {
    for (std::size_t i = 0; i < participants.size(); ++i) {
      if (participants[i].originator == originator_wanted) return i;
    }
    return std::nullopt;
  }
};

}  // namespace athenasip
