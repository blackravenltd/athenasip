//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "sip_header.h"
#include "util.h"

namespace athenasip {

class Channel;
class Call;
class Transaction;
class Subscriber;
class SIPUri;

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
  std::shared_ptr<Channel> channel;
  std::shared_ptr<Call> call;
  std::shared_ptr<Subscriber> subscriber;
  std::shared_ptr<SIPUri> contact;
  bool authenticated = false;
  std::shared_ptr<Transaction> transaction;

  // Methods
  std::string to_string() const;
  void print() const;
  std::string get_transaction_id();
  std::shared_ptr<SIPMessage> generate_response();

  friend std::string operator+(const SIPMessage& header, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPMessage& header);

 private:
  std::string generate_sip_tag();
};

}  // namespace athenasip