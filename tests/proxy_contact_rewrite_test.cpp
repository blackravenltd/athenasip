//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "headers/sip_identity_header.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;
using athenasip::headers::SIPIdentityHeader;

namespace {

// Sets the Contact, e.g. to the private address a phone behind NAT writes.
std::string with_contact(std::string raw, const std::string& contact) {
  const auto start = raw.find("Contact: ");
  const auto end = raw.find("\r\n", start);
  return raw.replace(start, end - start, "Contact: <" + contact + ">");
}

std::string contact_of(const std::shared_ptr<SIPMessage>& message) {
  if (!message || !message->header->contains("Contact")) return "";
  auto contact = message->header->headers_map["Contact"][0]->as<SIPIdentityHeader>();
  return contact && contact->value && contact->value->uri ? contact->value->uri->to_string() : "";
}

struct RewriteFixture : ProxyFixture {
  RewriteFixture() { bind_bob(); }

  void set_realm(std::optional<bool> rewrite) {
    auto realm = store->realm_get_by_name("example.com");
    realm->behaviour.rewrite_contact = rewrite;
    store->realm_update(realm);
  }
};

}  // namespace

// RFC 3261 16.6: by default the Contact is forwarded as written.
TEST(ProxyContactRewriteTest, TheContactIsLeftAloneByDefault) {
  RewriteFixture f;

  f.receive(f.caller, with_contact(f.invite(), "sip:alice@10.0.0.9:5060"));

  EXPECT_EQ(contact_of(f.request_with(f.callee_connection, "INVITE")), "sip:alice@10.0.0.9:5060");
}

// With rewrite_contact on, the Contact becomes the address the message came from, so
// in-dialog requests reach the NAT mapping.
TEST(ProxyContactRewriteTest, TurnedOnTheContactBecomesWhereTheRequestCameFrom) {
  RewriteFixture f;
  f.config->behaviour_rewrite_contact = true;

  f.receive(f.caller, with_contact(f.invite(), "sip:alice@10.0.0.9:5070;transport=udp"));

  // Only host and port change; the user and parameters are kept.
  EXPECT_EQ(contact_of(f.request_with(f.callee_connection, "INVITE")), "sip:alice@192.0.2.10:5060;transport=udp");
}

// A response's Contact is rewritten too.
TEST(ProxyContactRewriteTest, TheAnswersContactIsRewrittenToo) {
  RewriteFixture f;
  f.config->behaviour_rewrite_contact = true;

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:bob@192.168.1.5:5060"));

  EXPECT_EQ(contact_of(f.response_with(f.caller_connection, 200)), "sip:bob@192.0.2.20:5060");
}

// A realm setting overrides the node default.
TEST(ProxyContactRewriteTest, ARealmCanTurnItOff) {
  RewriteFixture f;
  f.config->behaviour_rewrite_contact = true;
  f.set_realm(false);

  f.receive(f.caller, with_contact(f.invite(), "sip:alice@10.0.0.9:5060"));

  EXPECT_EQ(contact_of(f.request_with(f.callee_connection, "INVITE")), "sip:alice@10.0.0.9:5060");
}

// RFC 7118: a WebSocket client's Contact is unroutable by design and it is reached by
// its flow, so the Contact is left alone.
TEST(ProxyContactRewriteTest, AWebSocketClientsContactIsLeftAlone) {
  RewriteFixture f;
  f.config->behaviour_rewrite_contact = true;

  std::shared_ptr<MockConnection> browser_connection;
  auto browser = f.make_channel("192.0.2.30", &browser_connection, "wss", 51234);
  f.on_strand([&browser]() { browser->authenticated_as("sip:alice@example.com"); });

  f.receive(browser, with_contact(f.invite(), "sip:k3x9@df7jal23ls0d.invalid;transport=ws"));

  EXPECT_EQ(contact_of(f.request_with(f.callee_connection, "INVITE")), "sip:k3x9@df7jal23ls0d.invalid;transport=ws");
}
