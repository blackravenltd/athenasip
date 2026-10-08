//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "sip_header.h"
#include "types/sip_uri.h"
#include "types/subscriber.h"
#include "util.h"

using namespace athenasip::types;

namespace athenasip {

namespace types {
struct Dialog;
}

class Channel;
class Call;

class SIPMessage {
 public:
  SIPMessage();

  std::shared_ptr<SIPHeader> header;
  std::string body;
  uint body_length = 0;

  std::string branch;
  uint16_t source_port;
  std::weak_ptr<Channel> channel;

  // The flow token from a Route naming this node, if any (RFC 5626 section 5.1). It routes
  // an in-dialog request when the route set is spent and the Contact does not resolve.
  std::string flow_token;
  std::weak_ptr<Call> call;
  std::weak_ptr<Subscriber> subscriber;
  std::shared_ptr<SIPUri> contact;
  bool authenticated = false;

  // The trunk the policy trusted this request as coming from, by name; empty for anything else.
  std::string trunk;

  // The request belonged to a known dialog when it arrived (RFC 3261 12.2.2). Recorded
  // before the dialog table sees it, because a BYE removes its dialog there.
  bool in_known_dialog = false;

  // That dialog, held from before the dialog table saw the request: a BYE has ended it there by the time the
  // request is routed, and what the dialog recorded is still needed to forward the BYE.
  std::shared_ptr<types::Dialog> dialog;

  std::string to_string() const;
  void print() const;
  std::string get_transaction_id();
  std::shared_ptr<SIPMessage> generate_response();

  // RFC 3261 16.6 step 1: a proxy forwards a copy of the request. A deep copy, because
  // header values are shared and a Via added for one branch would appear on the next.
  std::shared_ptr<SIPMessage> clone() const;

  friend std::string operator+(const SIPMessage& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPMessage& header);

 private:
  std::string generate_sip_tag();
};

}  // namespace athenasip