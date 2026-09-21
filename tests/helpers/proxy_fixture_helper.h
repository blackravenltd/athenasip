//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core_fixture_helper.h"

// A node with two accounts on it, Alice calling and Bob answering, driven as a
// transport drives it. Every channel the mock builds is local to 192.0.2.1:5060, which
// is what the node's own Record-Route and Via name and what a Route coming back has to
// be recognised against.
struct ProxyFixture : CoreFixture {
  std::shared_ptr<MockConnection> caller_connection;
  std::shared_ptr<athenasip::Channel> caller;

  std::shared_ptr<MockConnection> callee_connection;
  std::shared_ptr<athenasip::Channel> callee;

  std::shared_ptr<athenasip::types::Account> bob;

  ProxyFixture() {
    seed_realm("example.com");
    seed_account(1, "sip:alice@example.com", "alice-ha1");
    bob = seed_account(2, "sip:bob@example.com", "bob-ha1");

    caller = make_channel("192.0.2.10", &caller_connection);
    callee = make_channel("192.0.2.20", &callee_connection);
  }

  // Puts Bob on the callee channel, the way a successful REGISTER would.
  void bind_bob(const std::string& contact = "sip:bob@192.0.2.20:5060") {
    register_binding(bob, std::make_shared<athenasip::types::SIPUri>(contact), callee, 3600);
  }

  std::string invite(const std::string& branch = "z9hG4bK-invite", const std::string& to = "sip:bob@example.com", const std::string& max_forwards = "70") {
    std::string raw = "INVITE " + to + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <" + to + ">\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    if (!max_forwards.empty()) raw += "Max-Forwards: " + max_forwards + "\r\n";
    raw += "\r\n";
    return raw;
  }

  // The response the callee sends back, carrying the Via chain and the Record-Route the
  // proxy built. RFC 3261 12.1.1: a UAS copies Record-Route into its response in the
  // order it arrived, and offers its own Contact, which is how the caller learns the
  // route set and the remote target.
  std::string response_from_callee(int code, const std::string& reason, const std::string& to_tag = "bob",
                                   const std::string& contact = "sip:bob@192.0.2.20:5060", const std::string& extra = "") {
    auto forwarded = request_with(callee_connection, "INVITE");
    if (!forwarded) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    for (const auto& route : forwarded->header->headers_map["Record-Route"]) raw += "Record-Route: " + route->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=" + to_tag + "\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    if (!contact.empty()) raw += "Contact: <" + contact + ">\r\n";
    raw += extra;
    raw += "\r\n";
    return raw;
  }

  // The dialogs this node is still on the path of, read from the strand that owns them.
  std::vector<std::shared_ptr<athenasip::types::Dialog>> dialogs() {
    return on_strand([this]() { return core->dialogs()->all(); });
  }

  std::shared_ptr<athenasip::types::Dialog> only_dialog() {
    auto found = dialogs();
    return found.size() == 1 ? found.front() : nullptr;
  }

  std::shared_ptr<athenasip::Call> call(const std::string& id = "call-proxy") {
    return on_strand([this, id]() { return core->call_get(id); });
  }

  // Every request of this method written to a connection, in the order they went out.
  static std::vector<std::shared_ptr<athenasip::SIPMessage>> requests_with(const std::shared_ptr<MockConnection>& connection, const std::string& method) {
    std::vector<std::shared_ptr<athenasip::SIPMessage>> found;

    for (const auto& message : written(connection)) {
      if (message->header->type == athenasip::SIPHeader::Type::Request && message->header->request_method == method) found.push_back(message);
    }

    return found;
  }
};
