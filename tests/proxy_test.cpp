//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/json.hpp>

#include "types/trunk.h"

#include <memory>
#include <string>

#include "headers/cseq_header.h"
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

// Review finding 16: a Max-Forwards that is no number cannot be decremented, so the request is refused rather than
// forwarded with its hop count intact.
TEST(ProxyTest, AMaxForwardsThatIsNoNumberIs400AndIsNotForwarded) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:bob@example.com", "seventy"));

  EXPECT_NE(f.response_with(f.caller_connection, 400), nullptr);
  EXPECT_EQ(f.request_with(f.callee_connection, "INVITE"), nullptr);
}

// RFC 3261 16.3 step 5, review finding 12: a request that requires of proxies an extension this one lacks is 420
// Bad Extension, with each tag in Unsupported, and is not forwarded.
TEST(ProxyTest, AProxyRequireThisProxyLacksIs420WithUnsupported) {
  Fixture f;
  f.bind_bob();

  auto invite = f.invite("z9hG4bK-invite", "sip:bob@example.com");
  invite.insert(invite.find("Max-Forwards:"), "Proxy-Require: foo, bar\r\n");
  f.receive(f.caller, invite);

  auto refused = f.response_with(f.caller_connection, 420);
  ASSERT_NE(refused, nullptr);
  ASSERT_EQ(refused->header->headers_map["Unsupported"].size(), 2u);
  EXPECT_EQ(refused->header->headers_map["Unsupported"][0]->to_string(), "foo");
  EXPECT_EQ(refused->header->headers_map["Unsupported"][1]->to_string(), "bar");
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
  const auto invite = f.with_credentials(f.invite("z9hG4bK-twice"), "alice", "alice-ha1", "192.0.2.11");

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

// RFC 3261 9.2: the 487 is the INVITE's response, so it carries the INVITE's CSeq; and the To tag of the response to
// the CANCEL and of the response to the INVITE SHOULD be the same. A caller matches the 487 to its INVITE by the
// CSeq method (17.1.3), and one that cannot never sends the ACK: found against a carrier, which let the node
// retransmit the 487 until timer H.
TEST(ProxyTest, The487IsTheInvitesResponseWithTheTagOfTheCancels200) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);
  const auto invite_cseq = f.request_with(f.callee_connection, "INVITE")->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>()->sequence;

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: " + std::to_string(invite_cseq) + " CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);

  const auto ok = f.response_with(f.caller_connection, 200);
  const auto terminated = f.response_with(f.caller_connection, 487);
  ASSERT_NE(ok, nullptr);
  ASSERT_NE(terminated, nullptr);

  const auto cseq_of = [](const std::shared_ptr<SIPMessage>& message) { return message->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>(); };
  EXPECT_EQ(cseq_of(terminated)->method, "INVITE");
  EXPECT_EQ(cseq_of(terminated)->sequence, invite_cseq);
  EXPECT_EQ(cseq_of(ok)->method, "CANCEL") << "the CANCEL's own response keeps its method";

  const auto to_tag = [](const std::shared_ptr<SIPMessage>& message) {
    return message->header->headers_map["To"][0]->as<headers::SIPIdentityHeader>()->value->tags.at("tag");
  };
  EXPECT_EQ(to_tag(terminated), to_tag(ok));
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

// RFC 3261 16.7 step 5: every provisional response but 100 is forwarded. The callee's 100 is hop-by-hop; the
// caller has had this node's own.
TEST(ProxyTest, TheCalleesTryingIsNotForwarded) {
  ProxyFixture f;
  f.bind_bob();
  f.receive(f.caller, f.invite());

  const auto trying_before = ProxyFixture::responses_with(f.caller_connection, 100).size();
  f.receive(f.callee, f.response_from_callee(100, "Trying", ""));
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  EXPECT_EQ(ProxyFixture::responses_with(f.caller_connection, 100).size(), trying_before);
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 180), nullptr);
}

// RFC 3261 19.1.4: the host of a Request-URI compares without regard to case, so a call dialled with the realm in
// capitals reaches the subscriber as one in lower case does.
TEST(ProxyTest, ARequestUriHostInAnyCaseReachesTheSubscriber) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-case", "sip:bob@EXAMPLE.COM"));

  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 404), nullptr);
}

// RFC 3428: a MESSAGE out of a dialog is proxied like any request, to the subscriber's device, with its body
// untouched; the device's final response goes back to the sender. No dialog and no Record-Route, since none is
// created, and nothing is stored: a subscriber with no device registered is told 480.
TEST(ProxyTest, AnInstantMessageIsRelayedWithItsBodyAndItsAnswer) {
  ProxyFixture f;
  f.bind_bob();

  const std::string text = "Are you there?";
  std::string raw = "MESSAGE sip:bob@example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-im\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>\r\n";
  raw += "Call-ID: im-1\r\n";
  raw += "CSeq: 1 MESSAGE\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "Content-Type: text/plain\r\n";
  raw += "Content-Length: " + std::to_string(text.size()) + "\r\n\r\n";
  raw += text;
  f.receive(f.caller, raw);

  const auto forwarded = ProxyFixture::requests_with(f.callee_connection, "MESSAGE");
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded[0]->body, text);
  EXPECT_EQ(forwarded[0]->header->headers_map["Content-Type"][0]->to_string(), "text/plain");
  EXPECT_FALSE(forwarded[0]->header->contains("Record-Route"));

  std::string answer = "SIP/2.0 200 OK\r\n";
  for (const auto& via : forwarded[0]->header->headers_map["Via"]) answer += "Via: " + via->to_string() + "\r\n";
  answer += "From: <sip:alice@example.com>;tag=alice\r\n";
  answer += "To: <sip:bob@example.com>;tag=bob\r\n";
  answer += "Call-ID: im-1\r\n";
  answer += "CSeq: 1 MESSAGE\r\n\r\n";
  f.receive(f.callee, answer);

  auto relayed = ProxyFixture::response_with(f.caller_connection, 200);
  ASSERT_NE(relayed, nullptr);
  EXPECT_EQ(relayed->header->headers_map["CSeq"][0]->to_string(), "1 MESSAGE");

  // Bob gone: nobody to deliver to.
  ASSERT_TRUE(f.unregister_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), f.callee));
  raw.replace(raw.find("z9hG4bK-im"), 10, "z9hG4bK-i2");
  raw.replace(raw.find("Call-ID: im-1"), 13, "Call-ID: im-2");
  f.receive(f.caller, raw);
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 480), nullptr);
}

// RFC 4579 (docs/design/conferencing.md): an INVITE to a conference URI goes to its focus, a trunk, with the room
// as the user part, and the call records the room as its focus once the focus answers, by its name rather than as
// this caller spelt it. Conferencing is the standard scripts' (decision 2), so this is proved under them.
TEST(ProxyTest, ACallToAConferenceUriGoesToItsFocusAndTheCallRecordsTheRoom) {
  if (!CoreFixture::lua_policy()) GTEST_SKIP() << "conferencing is athenasip.standard's; run with ATHENA_TEST_POLICY=lua";
  ProxyFixture f;
  std::shared_ptr<MockConnection> focus_connection;
  auto focus = f.make_channel("192.0.2.40", &focus_connection);

  auto rooms = std::make_shared<types::Trunk>();
  rooms->name = "rooms";
  rooms->uri = "sip:192.0.2.40:5060";
  rooms->attributes = boost::json::parse(R"({"focus": {"prefix": "conf-"}})").as_object();
  ASSERT_TRUE(f.store->trunk_create(rooms));

  f.receive(f.caller, f.invite("z9hG4bK-room", "sip:conf-sales@EXAMPLE.com;transport=udp"));

  const auto forwarded = ProxyFixture::requests_with(focus_connection, "INVITE");
  ASSERT_EQ(forwarded.size(), 1u);
  EXPECT_EQ(forwarded[0]->header->request_uri->to_string(), "sip:conf-sales@192.0.2.40:5060");
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 404), nullptr);

  std::string answer = "SIP/2.0 200 OK\r\n";
  for (const auto& via : forwarded[0]->header->headers_map["Via"]) answer += "Via: " + via->to_string() + "\r\n";
  for (const auto& route : forwarded[0]->header->headers_map["Record-Route"]) answer += "Record-Route: " + route->to_string() + "\r\n";
  answer += "From: <sip:alice@example.com>;tag=alice\r\n";
  answer += "To: <sip:conf-sales@example.com>;tag=focus\r\n";
  answer += "Call-ID: call-proxy\r\n";
  answer += "CSeq: 1 INVITE\r\n";
  answer += "Contact: <sip:conf-sales@192.0.2.40:5060;isfocus>\r\n\r\n";
  f.receive(focus, answer);

  ASSERT_NE(ProxyFixture::response_with(f.caller_connection, 200), nullptr);
  const auto call = f.store->call_get("call-proxy");
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->focus, nullptr);
  EXPECT_EQ(call->focus->to_string(), "sip:conf-sales@example.com");
}
