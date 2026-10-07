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

// A call has any number of legs: two participants and no focus is a two-party call, more
// with a focus URI is a conference (RFC 4579).
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

  // One leg of the call. The channel is weak: a participant outlives its connection. A leg
  // on another cluster node has that node in node_id and no channel.
  struct Participant {
    std::shared_ptr<SIPIdentity> identity;
    std::weak_ptr<Channel> channel;
    std::string node_id;

    // The leg's dialog (RFC 3261 section 12). Both legs of a proxied two-party call share
    // one; a conference has one per participant, each with the focus (RFC 4579).
    std::shared_ptr<Dialog> dialog;

    // Media for this leg, keyed by the SDP media identifier.
    std::unordered_map<int64_t, std::shared_ptr<MediaStream>> streams;

    // The media profile from the leg's last offer or answer. It outranks the signalling
    // transport: WebRTC media can be signalled over UDP, and RFC 7118 allows a plain phone
    // on a WebSocket. Empty until the leg has described itself.
    std::optional<media::Profile> profile;

    // The subscriber's configured profile, used only while `profile` is empty. It outranks
    // the transport.
    std::optional<MediaPolicy::Profiles> subscriber_profile;

    // The party that started the call.
    bool originator = false;
  };

  std::string id;
  State state = State::Initial;

  // The conference focus this node routes to (RFC 4579). Empty for a two-party call.
  std::shared_ptr<SIPUri> focus;

  // The realm's media policy, fixed at call setup: a re-INVITE never looks the realm up,
  // and hold and resume must behave as the first INVITE did.
  MediaPolicy media_policy;

  // Whether Contacts are rewritten to the address messages came from. Fixed at call setup.
  std::optional<bool> rewrite_contact;

  // A peer node forwarded this call here and anchors its media, so this node leaves every
  // session description alone.
  bool media_elsewhere = false;

  // The call record (CDR). Only `node`, the node the caller reached, writes it. A node the
  // call was forwarded to has that peer in `from_node` and writes nothing. `media_engine`
  // is the engine that anchored the media, or empty.
  std::string node;
  std::string from_node;
  std::string media_engine;

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

  // The index of the originating leg, or of the first other leg. The media contract
  // addresses participants by index.
  std::optional<std::size_t> participant_index(bool originator_wanted) const {
    for (std::size_t i = 0; i < participants.size(); ++i) {
      if (participants[i].originator == originator_wanted) return i;
    }
    return std::nullopt;
  }
};

}  // namespace athenasip
