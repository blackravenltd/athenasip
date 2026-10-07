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

// The URI of a Route or Record-Route value.
std::shared_ptr<types::SIPUri> route_uri(const std::shared_ptr<SIPMessage>& message, const std::string& field, std::size_t index = 0) {
  if (!message->header->contains(field)) return nullptr;

  const auto& values = message->header->headers_map[field];
  if (index >= values.size()) return nullptr;

  auto identity = values[index]->as<SIPIdentityHeader>();
  if (identity == nullptr) return nullptr;

  return identity->value ? identity->value->uri : nullptr;
}

// An in-dialog request as an endpoint sends it: the route set it kept, and the remote
// target as the Request-URI.
std::string in_dialog(const std::string& method, const std::string& request_uri, const std::string& route, const std::string& branch, int cseq = 2) {
  std::string raw = method + " " + request_uri + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
  if (!route.empty()) raw += "Route: " + route + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>;tag=bob\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: " + std::to_string(cseq) + " " + method + "\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "\r\n";
  return raw;
}

}  // namespace

// RFC 3261 16.6 step 4: a dialog-creating request is Record-Routed, so the ACK and BYE
// come back through this node.
TEST(ProxyRouteTest, AnInviteIsRecordRouted) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto record_route = route_uri(forwarded, "Record-Route");
  ASSERT_NE(record_route, nullptr);

  EXPECT_EQ(record_route->host, "192.0.2.1");
  EXPECT_EQ(record_route->port.value_or(0), 5060);

  // RFC 3261 19.1.1: lr marks this node as a loose router.
  EXPECT_TRUE(record_route->has_parameter("lr"));
}

// RFC 3261 16.6 step 4: a request that creates no dialog is not Record-Routed.
TEST(ProxyRouteTest, ARequestThatCreatesNoDialogIsNotRecordRouted) {
  Fixture f;
  f.bind_bob();

  std::string message = "MESSAGE sip:bob@example.com SIP/2.0\r\n";
  message += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-message\r\n";
  message += "From: <sip:alice@example.com>;tag=alice\r\n";
  message += "To: <sip:bob@example.com>\r\n";
  message += "Call-ID: call-message\r\n";
  message += "CSeq: 1 MESSAGE\r\n";
  message += "Max-Forwards: 70\r\n";
  message += "\r\n";

  f.receive(f.caller, message);

  auto forwarded = f.request_with(f.callee_connection, "MESSAGE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_FALSE(forwarded->header->contains("Record-Route"));
}

// RFC 3261 16.4: a Route naming this node is removed, or the request would come back.
TEST(ProxyRouteTest, ARouteNamingThisNodeIsRemoved) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_FALSE(forwarded->header->contains("Route"));
}

// RFC 3261 16.5, 16.6: an in-dialog BYE is forwarded to its Request-URI, the remote
// target, on its route set. No dialog state is consulted (16.1).
TEST(ProxyRouteTest, AnInDialogByeTransitsOnItsRouteSet) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);

  // A loose route does not rewrite the Request-URI.
  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_EQ(forwarded->header->request_uri->user, "bob");

  // This node is on the path, so the Via chain is two deep.
  EXPECT_EQ(forwarded->header->headers_map["Via"].size(), 2u);
}

// RFC 3261 16.6 step 7, RFC 5923: a request to a hop this node already has a flow to
// reuses that flow. Behind NAT, the flow the client opened is the only way back.
TEST(ProxyRouteTest, ASecondInDialogRequestReusesTheFirstOnesFlow) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye-1"));
  ASSERT_NE(f.request_with(f.callee_connection, "BYE"), nullptr);

  const auto after_first = f.callee_connection->write_calls;
  ASSERT_GT(after_first, 0);

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye-2"));

  // The same connection carried it.
  EXPECT_GT(f.callee_connection->write_calls, after_first);

  // The registry still holds one channel for that hop, the original.
  auto found = f.on_strand([&f]() { return f.core->channel_find("udp", "192.0.2.20", 5060); });
  EXPECT_EQ(found, f.callee);
}

// RFC 3261 16.6 step 6: the top Route decides the hop and the Request-URI is left alone.
TEST(ProxyRouteTest, ATopRouteDecidesTheHopAndLeavesTheRequestUri) {
  Fixture f;

  f.receive(f.caller, in_dialog("INVITE", "sip:bob@example.com", "<sip:192.0.2.20:5060;lr>", "z9hG4bK-routed", 1));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "example.com");

  // The Route was not this node's, so it travels on with the request.
  auto remaining = route_uri(forwarded, "Route");
  ASSERT_NE(remaining, nullptr);
  EXPECT_EQ(remaining->host, "192.0.2.20");
}

// RFC 3261 16.6 step 6: a top Route without lr is a strict router's. Its URI becomes the
// Request-URI, and the old Request-URI goes to the end of the route set.
TEST(ProxyRouteTest, AStrictRouteIsRewrittenIntoTheRequestUri) {
  Fixture f;

  f.receive(f.caller, in_dialog("INVITE", "sip:bob@example.com", "<sip:192.0.2.20:5060>", "z9hG4bK-strict", 1));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_FALSE(forwarded->header->request_uri->has_parameter("lr"));

  // The address of record is now the last Route value.
  const auto& routes = forwarded->header->headers_map["Route"];
  ASSERT_FALSE(routes.empty());

  auto last = route_uri(forwarded, "Route", routes.size() - 1);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->host, "example.com");
  EXPECT_EQ(last->user, "bob");
}

// RFC 3261 16.4: a strict router upstream put this node's URI in the Request-URI and the
// real target last in the route set. Route preprocessing restores the target.
TEST(ProxyRouteTest, AStrictRoutersRewriteIsUndone) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:192.0.2.1:5060;lr", "<sip:bob@192.0.2.20:5060>", "z9hG4bK-strict-in"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_EQ(forwarded->header->request_uri->user, "bob");

  // The recovered value is not also sent on as a Route.
  EXPECT_FALSE(forwarded->header->contains("Route"));
}

// RFC 3261 16.5: a Request-URI in a domain this node does not serve is the only target;
// there is no location lookup.
TEST(ProxyRouteTest, ARequestForADomainWeDoNotServeGoesToItsRequestUri) {
  Fixture f;

  f.receive(f.caller, in_dialog("OPTIONS", "sip:carol@192.0.2.20:5060", "", "z9hG4bK-options", 1));

  auto forwarded = f.request_with(f.callee_connection, "OPTIONS");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->user, "carol");
}

// RFC 3261 16.6 step 1: each branch of a fork starts from a copy of the request as
// received, so Via and Max-Forwards do not accumulate across attempts.
TEST(ProxyRouteTest, EachForkStartsFromTheRequestAsReceived) {
  Fixture f;
  f.bind_bob("sip:bob@192.0.2.20:5060");
  f.bind_bob("sip:bob@192.0.2.20:5070");

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(486, "Busy Here"));

  auto forwarded = f.requests_with(f.callee_connection, "INVITE");
  ASSERT_EQ(forwarded.size(), 2u);

  for (const auto& attempt : forwarded) {
    EXPECT_EQ(attempt->header->headers_map["Via"].size(), 2u);
    EXPECT_EQ(attempt->header->headers_map["Max-Forwards"][0]->as<UIntHeader>()->value, 69u);
    // Two per attempt, not four: one branch's Record-Route pair (RFC 5658) is not
    // inherited by the next.
    EXPECT_EQ(attempt->header->headers_map["Record-Route"].size(), 2u);
  }

  // RFC 3261 16.6 step 8: two branches are two transactions.
  EXPECT_NE(forwarded[0]->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"],
            forwarded[1]->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"]);
}

// RFC 3261 16.3.4: the branch this node writes hashes the fields that decide routing, so
// a request that comes back unchanged is a loop and gets 482.
TEST(ProxyRouteTest, ARequestThatComesBackUnchangedIs482) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  // What a looping downstream element sends back: one more Via on top and the original
  // Request-URI.
  std::string looped = "INVITE sip:bob@example.com SIP/2.0\r\n";
  looped += "Via: SIP/2.0/UDP 192.0.2.99:5060;branch=z9hG4bK-elsewhere\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) looped += "Via: " + via->to_string() + "\r\n";
  looped += "From: <sip:alice@example.com>;tag=alice\r\n";
  looped += "To: <sip:bob@example.com>\r\n";
  looped += "Call-ID: call-proxy\r\n";
  looped += "CSeq: 1 INVITE\r\n";
  looped += "Max-Forwards: 40\r\n";
  looped += "\r\n";

  f.receive(f.caller, looped);

  EXPECT_NE(f.response_with(f.caller_connection, 482), nullptr);
}

// RFC 3261 16.3.4: the same Via with a different Request-URI is a spiral, and is allowed.
TEST(ProxyRouteTest, ARequestThatComesBackChangedIsASpiralAndNot482) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  std::string spiral = "INVITE sip:nobody@example.com SIP/2.0\r\n";
  spiral += "Via: SIP/2.0/UDP 192.0.2.99:5060;branch=z9hG4bK-elsewhere\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) spiral += "Via: " + via->to_string() + "\r\n";
  spiral += "From: <sip:alice@example.com>;tag=alice\r\n";
  spiral += "To: <sip:bob@example.com>\r\n";
  spiral += "Call-ID: call-proxy\r\n";
  spiral += "CSeq: 1 INVITE\r\n";
  spiral += "Max-Forwards: 40\r\n";
  spiral += "\r\n";

  f.receive(f.caller, spiral);

  EXPECT_EQ(f.response_with(f.caller_connection, 482), nullptr);

  // Processed as an ordinary request: nobody@example.com has no subscriber.
  EXPECT_NE(f.response_with(f.caller_connection, 404), nullptr);
}

// RFC 3261 16.7 step 1, 18.1.2: a response matching no client transaction is forwarded
// statelessly down the Via chain with this node's Via removed.
TEST(ProxyRouteTest, AResponseWithNoTransactionIsForwardedStatelessly) {
  Fixture f;

  std::string stray = "SIP/2.0 200 OK\r\n";
  stray += "Via: SIP/2.0/UDP 192.0.2.1:5060;branch=z9hG4bK-gone\r\n";
  stray += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-caller\r\n";
  stray += "From: <sip:alice@example.com>;tag=alice\r\n";
  stray += "To: <sip:bob@example.com>;tag=bob\r\n";
  stray += "Call-ID: call-stray\r\n";
  stray += "CSeq: 1 INVITE\r\n";
  stray += "\r\n";

  f.receive(f.callee, stray);

  auto forwarded = f.response_with(f.caller_connection, 200);
  ASSERT_NE(forwarded, nullptr);

  const auto& vias = forwarded->header->headers_map["Via"];
  ASSERT_EQ(vias.size(), 1u);
  EXPECT_EQ(vias[0]->as<ViaHeader>()->parameters["branch"], "z9hG4bK-caller");
}

// RFC 3261 18.1.2: a response whose top Via is not this node's is dropped.
TEST(ProxyRouteTest, AResponseWhoseTopViaIsNotOursIsDropped) {
  Fixture f;

  std::string stray = "SIP/2.0 200 OK\r\n";
  stray += "Via: SIP/2.0/UDP 198.51.100.7:5060;branch=z9hG4bK-someone-else\r\n";
  stray += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-caller\r\n";
  stray += "From: <sip:alice@example.com>;tag=alice\r\n";
  stray += "To: <sip:bob@example.com>;tag=bob\r\n";
  stray += "Call-ID: call-stray\r\n";
  stray += "CSeq: 1 INVITE\r\n";
  stray += "\r\n";

  f.receive(f.callee, stray);

  EXPECT_EQ(f.response_with(f.caller_connection, 200), nullptr);
}

// RFC 3261 16.10: a CANCEL is forwarded to every branch already tried.
TEST(ProxyRouteTest, ACancelIsForwardedToTheBranchAlreadyTried) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 1 CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);

  auto sent = f.request_with(f.callee_connection, "CANCEL");
  ASSERT_NE(sent, nullptr);

  // RFC 3261 9.1: a single Via, matching the INVITE being cancelled.
  const auto& vias = sent->header->headers_map["Via"];
  ASSERT_EQ(vias.size(), 1u);
  EXPECT_EQ(vias[0]->as<ViaHeader>()->parameters["branch"], forwarded->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"]);
}

// RFC 3261 9.1: a CANCEL is held until the branch has answered provisionally.
TEST(ProxyRouteTest, ACancelWaitsForAProvisionalResponseOnTheBranch) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 1 CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);

  // No provisional response yet, so no CANCEL on the branch.
  EXPECT_EQ(f.request_with(f.callee_connection, "CANCEL"), nullptr);

  // The caller is answered straight away regardless.
  EXPECT_NE(f.response_with(f.caller_connection, 487), nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  EXPECT_NE(f.request_with(f.callee_connection, "CANCEL"), nullptr);
}

// RFC 3261 16.10: after a CANCEL the fork tries no further targets.
TEST(ProxyRouteTest, ACancelStopsTheForkTryingFurtherTargets) {
  Fixture f;
  f.bind_bob("sip:bob@192.0.2.20:5060");
  f.bind_bob("sip:bob@192.0.2.20:5070");

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  cancel += "From: <sip:alice@example.com>;tag=alice\r\n";
  cancel += "To: <sip:bob@example.com>\r\n";
  cancel += "Call-ID: call-proxy\r\n";
  cancel += "CSeq: 1 CANCEL\r\n";
  cancel += "\r\n";

  f.receive(f.caller, cancel);
  f.receive(f.callee, f.response_from_callee(487, "Request Terminated"));

  EXPECT_EQ(f.requests_with(f.callee_connection, "INVITE").size(), 1u);
}

// RFC 3261 16.7 step 6: a 503 is about the next hop, so it goes upstream as a 500.
TEST(ProxyRouteTest, A503FromTheOnlyBranchBecomesA500) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(503, "Service Unavailable"));

  EXPECT_NE(f.response_with(f.caller_connection, 500), nullptr);
  EXPECT_EQ(f.response_with(f.caller_connection, 503), nullptr);
}
