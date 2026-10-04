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
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;
using athenasip::headers::ViaHeader;

namespace {

using Fixture = ProxyFixture;

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

// RFC 3261 16.6 step 2: the forwarded Request-URI is the binding, not the address of record.
TEST(ProxyTest, TheForwardedRequestUriIsTheBinding) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
}

// RFC 3261 16.6 step 8: the proxy adds its own Via, with its own branch, above the caller's.
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

  // RFC 3261 8.1.1.7: the branch carries the magic cookie.
  EXPECT_EQ(top->parameters["branch"].rfind("z9hG4bK", 0), 0u);
  EXPECT_NE(top->parameters["branch"], "z9hG4bK-invite");

  // The caller's Via is untouched beneath it.
  EXPECT_EQ(below->parameters["branch"], "z9hG4bK-invite");
}

// RFC 3261 18.1.1: the Via transport is that of the outbound flow, not the inbound one.
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

// RFC 3261 16.6 step 3: Max-Forwards is decremented on forward.
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

// RFC 3261 16.3 rule 3: Max-Forwards of zero is answered 483 and not forwarded.
TEST(ProxyTest, MaxForwardsZeroIs483AndIsNotForwarded) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", "0"));

  EXPECT_NE(f.response_with(f.caller_connection, 483), nullptr);
  EXPECT_EQ(f.request_with(f.callee_connection, "INVITE"), nullptr);
}

// RFC 3261 16.6 step 3: a missing Max-Forwards is added with the default.
TEST(ProxyTest, AMissingMaxForwardsIsSuppliedOnForward) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", ""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  ASSERT_TRUE(forwarded->header->contains("Max-Forwards"));
  EXPECT_EQ(forwarded->header->headers_map["Max-Forwards"][0]->as<UIntHeader>()->value, 70u);
}

// RFC 3261 16.7 step 3: the proxy removes its own Via from the response.
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

// RFC 3261 17.2.1: the server transaction absorbs a retransmitted INVITE; it is not
// forwarded twice.
TEST(ProxyTest, ARetransmittedInviteIsNotForwardedTwice) {
  Fixture f;
  f.bind_bob();

  std::shared_ptr<MockConnection> caller_connection;
  auto caller = f.make_channel("192.0.2.11", &caller_connection);
  caller_connection->reliable = false;

  // On UDP Alice answers a challenge, and the retransmission carries the same credentials.
  const auto invite = f.with_credentials(f.invite("z9hG4bK-twice"), "alice", "alice-ha1");

  f.receive(caller, invite);
  f.receive(caller, invite);

  int forwarded = 0;
  for (const auto& message : f.written(f.callee_connection)) {
    if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE") forwarded++;
  }

  EXPECT_EQ(forwarded, 1);
}

// RFC 3261 9.2: the CANCEL is answered 200 and the INVITE it names ends with 487.
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

namespace {

std::string cancel_for(const std::string& request_uri, const std::string& branch = "z9hG4bK-nothing") {
  std::string cancel = "CANCEL " + request_uri + " SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 2 CANCEL\r\n";
  cancel += "Max-Forwards: 70\r\n";
  cancel += "\r\n";
  return cancel;
}

}  // namespace

// RFC 3261 16.10: a CANCEL with no response context is forwarded statelessly.
TEST(ProxyTest, ACancelWithNoContextIsForwardedOnwards) {
  Fixture f;

  f.receive(f.caller, cancel_for("sip:bob@192.0.2.20:5060"));

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "CANCEL");
  ASSERT_NE(forwarded, nullptr);

  // RFC 3261 16.7 step 1: this node's Via is on top, so the answer finds its way back.
  ASSERT_EQ(forwarded->header->headers_map["Via"].size(), 2u);
  EXPECT_EQ(forwarded->header->headers_map["Via"][0]->as<ViaHeader>()->host.rfind("192.0.2.1", 0), 0u);

  EXPECT_EQ(forwarded->header->headers_map["Max-Forwards"][0]->as<UIntHeader>()->value, 69u);

  // This node does not answer it itself.
  EXPECT_EQ(f.response_with(f.caller_connection, 200), nullptr);
}

// RFC 3261 17.2.2: the CANCEL's own server transaction absorbs retransmissions, so it is
// forwarded once. Its forwarded branch is derived from the request, not random (16.11).
TEST(ProxyTest, ARetransmittedCancelIsForwardedOnce) {
  Fixture f;

  f.caller_connection->reliable = false;

  f.receive(f.caller, cancel_for("sip:bob@192.0.2.20:5060"));
  f.receive(f.caller, cancel_for("sip:bob@192.0.2.20:5060"));

  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "CANCEL").size(), 1u);
}

// RFC 3261 9.2: a CANCEL with no context and nowhere to forward it is answered 481.
TEST(ProxyTest, ACancelWithNoContextAndNowhereToGoIs481) {
  Fixture f;

  f.receive(f.caller, cancel_for("sip:bob@example.com"));

  EXPECT_NE(f.response_with(f.caller_connection, 481), nullptr);
  EXPECT_EQ(f.response_with(f.caller_connection, 200), nullptr);
}

// RFC 3261 16.6: bindings are tried in turn; one failing is not the caller's answer
// while another is untried.
TEST(ProxyTest, SerialForkingTriesTheNextBindingOnAFailure) {
  Fixture f;
  f.bind_bob("sip:bob@192.0.2.20:5060");
  f.bind_bob("sip:bob@192.0.2.20:5070");

  f.receive(f.caller, f.invite());

  // Both bindings share the one registered flow.
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(486, "Busy Here"));

  int forwarded = 0;
  for (const auto& message : f.written(f.callee_connection)) {
    if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE") forwarded++;
  }

  EXPECT_EQ(forwarded, 2);

  // The second branch is still open, so the caller is not answered yet.
  EXPECT_EQ(f.response_with(f.caller_connection, 486), nullptr);
}

// RFC 3261 17.2.1: the server transaction absorbs the caller's ACK for a non-2xx. The
// one ACK the callee gets is the proxy's client transaction acknowledging the 486
// (17.1.1.3), on a different branch.
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

  // One binding, so the caller gets the 486; the client transaction has already
  // acknowledged it towards the callee.
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

// A listener bound to the wildcard has local endpoint 0.0.0.0. The Via and Record-Route
// carry the advertised address instead, since responses and in-dialog requests route by them.
TEST(ProxyTest, TheViaAndRecordRouteNameTheAddressThisNodeAdvertises) {
  ProxyFixture f("203.0.113.5");
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto top = forwarded->header->headers_map["Via"][0]->as<ViaHeader>();
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(top->host.rfind("203.0.113.5:", 0), 0u) << "Via sent-by was " << top->host;

  ASSERT_TRUE(forwarded->header->contains("Record-Route"));
  auto record_route = forwarded->header->headers_map["Record-Route"][0]->as<SIPIdentityHeader>();
  ASSERT_NE(record_route, nullptr);
  ASSERT_NE(record_route->value->uri, nullptr);
  EXPECT_EQ(record_route->value->uri->host, "203.0.113.5");
}

// RFC 3261 16.4: a Route carrying the advertised address names this node and is removed.
// Otherwise the request is forwarded to itself and refused as a loop (16.3.4).
TEST(ProxyTest, TheAdvertisedAddressIsRecognisedAsThisNode) {
  ProxyFixture f("203.0.113.5");
  f.bind_bob();

  f.receive(f.caller, f.invite());
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  auto record_route = forwarded->header->headers_map["Record-Route"][0]->as<SIPIdentityHeader>();
  ASSERT_NE(record_route, nullptr);

  const auto port = record_route->value->uri->port.value_or(5060);
  EXPECT_TRUE(f.core->is_local_address("203.0.113.5", port));
}

// RFC 6026 8.4, RFC 3261 16.7 step 5: every 2xx to an INVITE is forwarded, retransmissions
// included, down the connection the INVITE arrived on.
TEST(ProxyTest, ARetransmittedTwoHundredReachesTheCallerToo) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  ASSERT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);

  const auto ok = f.response_from_callee(200, "OK");
  f.receive(f.callee, ok);
  f.receive(f.callee, ok);

  int forwarded = 0;
  for (const auto& message : CoreFixture::written(f.caller_connection)) {
    if (message->header->type == athenasip::SIPHeader::Type::Response && message->header->response_code == 200) forwarded++;
  }
  EXPECT_EQ(forwarded, 2);
}
