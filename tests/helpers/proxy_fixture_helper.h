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
#include "util.h"

// A node with two subscribers, Alice calling and Bob answering. Every mock channel is local to 192.0.2.1:5060,
// the address the node's own Record-Route and Via name.
struct ProxyFixture : CoreFixture {
  std::shared_ptr<MockConnection> caller_connection;
  std::shared_ptr<athenasip::Channel> caller;

  std::shared_ptr<MockConnection> callee_connection;
  std::shared_ptr<athenasip::Channel> callee;

  std::shared_ptr<athenasip::types::Subscriber> alice;
  std::shared_ptr<athenasip::types::Subscriber> bob;

  // The public address is set before any channel opens, as at node start: a flow's registered address depends on it.
  explicit ProxyFixture(const std::string& public_address = "") {
    if (!public_address.empty()) config->sip_public_address = public_address;

    seed_realm("example.com");
    alice = seed_subscriber(1, "sip:alice@example.com", "alice-ha1");
    bob = seed_subscriber(2, "sip:bob@example.com", "bob-ha1");

    caller = make_channel("192.0.2.10", &caller_connection);
    callee = make_channel("192.0.2.20", &callee_connection);

    // Alice has registered over her connection, which must be a reliable one, so her calls are not challenged
    // (RFC 3261 22.3). The challenge itself is tested in tests/proxy_authentication_test.
    on_strand([this]() { caller->authenticated_as("sip:alice@example.com"); });
  }

  // Puts Bob on the callee channel, as an authenticated REGISTER would.
  void bind_bob(const std::string& contact = "sip:bob@192.0.2.20:5060") {
    register_binding(bob, std::make_shared<athenasip::types::SIPUri>(contact), callee, 3600);
    on_strand([this]() { callee->authenticated_as("sip:bob@example.com"); });
  }

  // The request with a Proxy-Authorization answering this node's 407 (RFC 3261 22.3): RFC 2617 digest without qop.
  std::string with_credentials(const std::string& raw, const std::string& user, const std::string& ha1) {
    const auto nonce = mint_nonce(store->realm_get_by_name("example.com"));

    const auto line_end = raw.find("\r\n");
    const auto first = raw.substr(0, line_end);
    const auto method = first.substr(0, first.find(' '));
    const auto uri = first.substr(method.size() + 1, first.rfind(' ') - method.size() - 1);

    const auto response = athenasip::Util::md5(ha1 + ":" + nonce + ":" + athenasip::Util::md5(method + ":" + uri));
    const auto header = "Proxy-Authorization: Digest username=\"" + user + "\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri +
                        "\", response=\"" + response + "\", algorithm=MD5\r\n";

    auto out = raw;
    out.insert(line_end + 2, header);
    return out;
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

  // The callee's response, carrying the Via chain and the Record-Route the proxy built, and its own Contact
  // (RFC 3261 12.1.1).
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
