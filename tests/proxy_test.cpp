//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "headers/uint_header.h"
#include "headers/via_header.h"

#include "helpers/core_fixture_helper.h"

using namespace athenasip;
using athenasip::headers::UIntHeader;
using athenasip::headers::ViaHeader;

namespace {

struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> caller_connection;
  std::shared_ptr<Channel> caller;

  std::shared_ptr<MockConnection> callee_connection;
  std::shared_ptr<Channel> callee;

  std::shared_ptr<types::Subscriber> bob;

  Fixture() {
    seed_realm("example.com");
    seed_subscriber(1, "sip:alice@example.com", "alice-ha1");
    bob = seed_subscriber(2, "sip:bob@example.com", "bob-ha1");

    caller = make_channel("192.0.2.10", &caller_connection);
    callee = make_channel("192.0.2.20", &callee_connection);
  }

  // Puts Bob on the callee channel, the way a successful REGISTER would.
  void bind_bob(const std::string& contact = "sip:bob@192.0.2.20:5060") {
    register_binding(bob, std::make_shared<types::SIPUri>(contact), callee, 3600);
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

  // The response the callee sends back, carrying the Via chain the proxy built.
  std::string response_from_callee(int code, const std::string& reason, const std::string& to_tag = "bob") {
    auto forwarded = request_with(callee_connection, "INVITE");
    if (!forwarded) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=" + to_tag + "\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// RFC 3261 16.5: a request for an address of record with no bindings cannot be routed.
TEST(ProxyTest, AnInviteForAnUnknownUserIs404) {
  Fixture f;

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:nobody@example.com"));

  EXPECT_NE(f.response_with(f.caller_connection, 404), nullptr);
}

TEST(ProxyTest, AnInviteForAUserWithNoBindingsIs480) {
  Fixture f;

  f.receive(f.caller, f.invite());

  EXPECT_NE(f.response_with(f.caller_connection, 480), nullptr);
}

// RFC 3261 16.6 step 2: the Request-URI becomes the binding being tried, not the address
// of record. The callee's own Contact is the only thing that reaches it.
TEST(ProxyTest, TheForwardedRequestUriIsTheBinding) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->realm, "192.0.2.20");
}

// RFC 3261 16.6 step 8: the proxy adds its own Via on top, with a branch of its own, and
// leaves the caller's beneath it. Without that the response cannot retrace the hops.
TEST(ProxyTest, TheProxyAddsItsOwnViaAboveTheCallers) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  const auto& vias = forwarded->header->headers_map["Via"];
  ASSERT_EQ(vias.size(), 2u);

  auto top = vias[0]->as<ViaHeader>();
  auto below = vias[1]->as<ViaHeader>();
  ASSERT_NE(top, nullptr);
  ASSERT_NE(below, nullptr);

  // RFC 3261 8.1.1.7: the branch is this node's own and carries the magic cookie.
  EXPECT_EQ(top->parameters["branch"].rfind("z9hG4bK", 0), 0u);
  EXPECT_NE(top->parameters["branch"], "z9hG4bK-invite");

  // The caller's Via survives underneath, untouched.
  EXPECT_EQ(below->parameters["branch"], "z9hG4bK-invite");
}

// RFC 3261 18.1.1: the Via transport is the one the request actually goes out on, which
// is a property of the outbound flow, not of the flow it arrived on.
TEST(ProxyTest, TheViaTransportIsTheOutboundOne) {
  Fixture f;

  std::shared_ptr<MockConnection> tls_connection;
  auto tls_channel = f.make_channel("192.0.2.30", &tls_connection, "tls");
  f.register_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.30:5061"), tls_channel, 3600);

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(tls_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto top = forwarded->header->headers_map["Via"][0]->as<ViaHeader>();
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(top->version, "SIP/2.0/TLS");
}

// RFC 3261 16.6 step 3: every hop decrements Max-Forwards, which is what stops a routing
// loop running forever.
TEST(ProxyTest, MaxForwardsIsDecrementedOnForward) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", "70"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Max-Forwards"));

  auto max_forwards = forwarded->header->headers_map["Max-Forwards"][0]->as<UIntHeader>();
  ASSERT_NE(max_forwards, nullptr);
  EXPECT_EQ(max_forwards->value, 69u);
}

// RFC 3261 16.3 rule 3: a request that has run out of hops is refused rather than
// forwarded one more time.
TEST(ProxyTest, MaxForwardsZeroIs483AndIsNotForwarded) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", "0"));

  EXPECT_NE(f.response_with(f.caller_connection, 483), nullptr);
  EXPECT_EQ(f.request_with(f.callee_connection, "INVITE"), nullptr);
}

// RFC 3261 16.6: a request arriving without Max-Forwards gets the default rather than
// travelling with no loop protection at all.
TEST(ProxyTest, AMissingMaxForwardsIsSuppliedOnForward) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", ""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Max-Forwards"));
  EXPECT_EQ(forwarded->header->headers_map["Max-Forwards"][0]->as<UIntHeader>()->value, 70u);
}

// RFC 3261 16.7 step 3: the proxy's own Via comes off the response, so the caller sees
// the chain it sent and nothing more.
TEST(ProxyTest, TheProxyStripsItsOwnViaFromTheResponse) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  auto ringing = f.response_with(f.caller_connection, 180);
  ASSERT_NE(ringing, nullptr);

  const auto& vias = ringing->header->headers_map["Via"];
  ASSERT_EQ(vias.size(), 1u);
  EXPECT_EQ(vias[0]->as<ViaHeader>()->parameters["branch"], "z9hG4bK-invite");
}

TEST(ProxyTest, A200TravelsBackToTheCaller) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));

  EXPECT_NE(f.response_with(f.caller_connection, 200), nullptr);
}

// RFC 3261 17.2.1: a retransmitted INVITE is absorbed by the server transaction, which
// answers it from what it last sent. Reaching the TU again would forward the call twice.
TEST(ProxyTest, ARetransmittedInviteIsNotForwardedTwice) {
  Fixture f;
  f.bind_bob();

  std::shared_ptr<MockConnection> caller_connection;
  auto caller = f.make_channel("192.0.2.11", &caller_connection);
  caller_connection->reliable = false;

  f.receive(caller, f.invite("z9hG4bK-twice"));
  f.receive(caller, f.invite("z9hG4bK-twice"));

  int forwarded = 0;
  for (const auto& message : f.written(f.callee_connection)) {
    if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE") forwarded++;
  }

  EXPECT_EQ(forwarded, 1);
}

// RFC 3261 9.2: the CANCEL is answered 200 on its own transaction, and the INVITE it
// names ends 487 so the caller stops waiting.
TEST(ProxyTest, ACancelAnswers200AndEndsTheInviteWith487) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 2 CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);

  EXPECT_NE(f.response_with(f.caller_connection, 200), nullptr);
  EXPECT_NE(f.response_with(f.caller_connection, 487), nullptr);
}

// A CANCEL that names nothing we hold is still answered, because the client is waiting
// on its own transaction for a reply either way.
TEST(ProxyTest, ACancelForAnUnknownTransactionIsStillAnswered) {
  Fixture f;

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-nothing\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 2 CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);

  EXPECT_NE(f.response_with(f.caller_connection, 200), nullptr);
}

// RFC 3261 16.6: with more than one binding the proxy tries them in turn, and a failure
// on one is not the answer to the caller while another is untried.
TEST(ProxyTest, SerialForkingTriesTheNextBindingOnAFailure) {
  Fixture f;
  f.bind_bob("sip:bob@192.0.2.20:5060");
  f.bind_bob("sip:bob@192.0.2.20:5070");

  f.receive(f.caller, f.invite());

  // Both bindings are on the one registered flow until RFC 5626 gives each its own.
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(486, "Busy Here"));

  int forwarded = 0;
  for (const auto& message : f.written(f.callee_connection)) {
    if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE") forwarded++;
  }

  EXPECT_EQ(forwarded, 2);

  // The caller has not been answered yet: the second branch is still open.
  EXPECT_EQ(f.response_with(f.caller_connection, 486), nullptr);
}

// RFC 3261 17.2.1: the ACK for a non-2xx is a transaction-level acknowledgement. The
// INVITE server transaction absorbs it and the proxy never sees it, so it is not passed
// on. The one ACK the callee does get is the proxy's own client transaction
// acknowledging the 486 it received (17.1.1.3), which is a different ACK on a different
// branch.
TEST(ProxyTest, TheAckForANonTwoHundredIsAbsorbedAndNotPassedOn) {
  Fixture f;
  f.bind_bob();

  auto acks_to_callee = [&f]() {
    int count = 0;
    for (const auto& message : f.written(f.callee_connection)) {
      if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "ACK") count++;
    }
    return count;
  };

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(486, "Busy Here"));

  // One binding, so the search is over and the caller gets the 486. The client
  // transaction has already acknowledged it towards the callee.
  ASSERT_NE(f.response_with(f.caller_connection, 486), nullptr);
  ASSERT_EQ(acks_to_callee(), 1);

  std::string ack = "ACK sip:bob@example.com SIP/2.0\r\n";
  ack += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  ack += "From: <sip:alice@example.com>;tag=alice\r\n";
  ack += "To: <sip:bob@example.com>;tag=bob\r\n";
  ack += "Call-ID: call-proxy\r\n";
  ack += "CSeq: 1 ACK\r\n";
  ack += "\r\n";

  f.receive(f.caller, ack);

  EXPECT_EQ(acks_to_callee(), 1);
}
