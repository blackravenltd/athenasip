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
#include "types/sip_identity.h"

using namespace athenasip::types;
using namespace athenasip::headers;

namespace athenasip {

class Session;

class Call {
 public:
  using SessionsFn = std::function<void(std::shared_ptr<Session> session)>;

  enum State { Initial, Trying, Ringing, Connected, Closing };

  Call(std::string callId) : id(callId) {}

  std::string id;

  std::shared_ptr<SIPIdentity> from;
  std::shared_ptr<SIPIdentity> to;

  void add_session(std::shared_ptr<Session> session) { _sessions[session] = true; }

  bool contains_session(std::shared_ptr<Session> session) { return _sessions[session]; }

  void with_all_sessions(SessionsFn callback) {
    for (const auto& pair : _sessions) callback(pair.first);
  }

  void with_all_sessions_except(SessionsFn callback, std::shared_ptr<Session> session) {
    for (const auto& pair : _sessions) {
      if (pair.first != session) callback(pair.first);
    }
  }

  State state = State::Initial;

 private:
  std::unordered_map<std::shared_ptr<Session>, bool> _sessions;
};

}  // namespace athenasip
