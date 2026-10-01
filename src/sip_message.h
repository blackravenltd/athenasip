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
#include "types/account.h"
#include "types/sip_uri.h"
#include "util.h"

using namespace athenasip::types;

namespace athenasip {

class Channel;
class Call;

class SIPMessage {
 public:
  SIPMessage();

  // Components of Message
  std::shared_ptr<SIPHeader> header;
  std::string body;
  uint body_length = 0;

  // Derived Information
  std::string branch;
  uint16_t source_port;
  std::weak_ptr<Channel> channel;

  // The flow token recovered from a Route naming this node, when there was one
  // (RFC 5626 section 5.1). It is what says where an in-dialog request goes once the
  // route set is spent and the Contact left over resolves to nothing.
  std::string flow_token;
  std::weak_ptr<Call> call;
  std::weak_ptr<Account> account;
  std::shared_ptr<SIPUri> contact;
  bool authenticated = false;

  // The request belonged to a dialog this node is on when it arrived (RFC 3261 12.2.2).
  // Taken before the dialog table sees it, because a BYE ends its dialog there and the
  // proxy asks afterwards.
  bool in_known_dialog = false;

  // Methods
  std::string to_string() const;
  void print() const;
  std::string get_transaction_id();
  std::shared_ptr<SIPMessage> generate_response();

  // RFC 3261 16.6 step 1: a proxy forwards a copy of the request it received, and keeps
  // the received one to answer with. Header values are polymorphic and shared, so a
  // shallow copy would put the Via added for one branch on the next one too.
  std::shared_ptr<SIPMessage> clone() const;

  friend std::string operator+(const SIPMessage& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPMessage& header);

 private:
  std::string generate_sip_tag();
};

}  // namespace athenasip