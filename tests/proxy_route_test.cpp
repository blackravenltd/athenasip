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

// The URI of a Route or Record-Route value, which is a name-addr like any other.
std::shared_ptr<types::SIPUri> route_uri(const std::shared_ptr<SIPMessage>& message, const std::string& field, std::size_t index = 0) {
  if (!message->header->contains(field)) return nullptr;

  const auto& values = message->header->headers_map[field];
  if (index >= values.size()) return nullptr;

  auto identity = values[index]->as<SIPIdentityHeader>();
  if (identity == nullptr) return nullptr;

  return identity->value ? identity->value->uri : nullptr;
}

// An in-dialog request as an endpoint that honoured a Record-Route would send it: the
// Route set it kept, and the remote target as the Request-URI.
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

// RFC 3261 16.6 step 4: a proxy that wants to stay on the path of a dialog says so on
// the request that creates it. Without this the ACK and the BYE go end to end and the
// node never learns the call ended, which a node that anchors media cannot afford.
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

  // 19.1.1: lr is what says this node is a loose router, and 16.6 step 4 requires it.
  EXPECT_TRUE(record_route->has_parameter("lr"));
}

// RFC 3261 16.6 step 4 applies to requests that create a dialog. A MESSAGE creates
// nothing to stay on the path of, so recording a route would only add a hop to a
// request that has no sequel.
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

// RFC 3261 16.4: a Route naming this node has been honoured by the request arriving
// here, so it comes off. Leaving it on would send the request back to this node.
TEST(ProxyRouteTest, ARouteNamingThisNodeIsRemoved) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_FALSE(forwarded->header->contains("Route"));
}

// RFC 3261 16.5 and 16.6: this is the whole of in-dialog routing. The Record-Route this
// node wrote brings the BYE back through it, and the Request-URI is the remote target,
// which is in no domain this node is responsible for, so it is forwarded as it stands.
// No dialog state is consulted, because a proxy is not dialog-stateful (16.1).
TEST(ProxyRouteTest, AnInDialogByeTransitsOnItsRouteSet) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);

  // The Request-URI is untouched: with a loose route the target is not rewritten.
  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_EQ(forwarded->header->request_uri->user, "bob");

  // And this node is on the path, so the Via chain is two deep.
  EXPECT_EQ(forwarded->header->headers_map["Via"].size(), 2u);
}

// RFC 3261 16.6 step 7 and RFC 5923: a second request to a hop this node already has a
// flow to goes out on that flow. Opening a second connection per request would leave a
// node holding one socket per in-dialog request, and for a client behind NAT the new
// one would not reach it at all - the flow the client opened is the only way back.
TEST(ProxyRouteTest, ASecondInDialogRequestReusesTheFirstOnesFlow) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye-1"));
  ASSERT_NE(f.request_with(f.callee_connection, "BYE"), nullptr);

  const auto after_first = f.callee_connection->write_calls;
  ASSERT_GT(after_first, 0);

  f.receive(f.caller, in_dialog("BYE", "sip:bob@192.0.2.20:5060", "<sip:192.0.2.1:5060;lr>", "z9hG4bK-bye-2"));

  // The same connection carried it: a second flow would have written somewhere else and
  // left this one where it was.
  EXPECT_GT(f.callee_connection->write_calls, after_first);

  // And the registry still holds one channel for that hop, which is the one we started
  // with.
  auto found = f.on_strand([&f]() { return f.core->channel_find("udp", "192.0.2.20", 5060); });
  EXPECT_EQ(found, f.callee);
}

// RFC 3261 16.6 step 6: with a route set in play the top Route decides the hop and the
// Request-URI is left alone. Here the Route names somewhere this node has a flow to and
// the Request-URI still names an address of record.
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

// RFC 3261 16.6 step 6: a top Route without lr belongs to a strict router, which expects
// to find its own URI in the Request-URI. The Request-URI being replaced goes to the end
// of the route set so the far end can put it back.
TEST(ProxyRouteTest, AStrictRouteIsRewrittenIntoTheRequestUri) {
  Fixture f;

  f.receive(f.caller, in_dialog("INVITE", "sip:bob@example.com", "<sip:192.0.2.20:5060>", "z9hG4bK-strict", 1));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_FALSE(forwarded->header->request_uri->has_parameter("lr"));

  // The address of record is now the last Route value rather than lost.
  const auto& routes = forwarded->header->headers_map["Route"];
  ASSERT_FALSE(routes.empty());

  auto last = route_uri(forwarded, "Route", routes.size() - 1);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->host, "example.com");
  EXPECT_EQ(last->user, "bob");
}

// RFC 3261 16.4: a strict router upstream will have moved this node's Record-Route into
// the Request-URI and pushed the real target to the end of the route set. Undoing that
// is the first thing route preprocessing does, and without it the request would be
// forwarded to this node's own address.
TEST(ProxyRouteTest, AStrictRoutersRewriteIsUndone) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, in_dialog("BYE", "sip:192.0.2.1:5060;lr", "<sip:bob@192.0.2.20:5060>", "z9hG4bK-strict-in"));

  auto forwarded = f.request_with(f.callee_connection, "BYE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(forwarded->header->request_uri, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.0.2.20");
  EXPECT_EQ(forwarded->header->request_uri->user, "bob");

  // The route set is spent: the value that was recovered is not sent on as well.
  EXPECT_FALSE(forwarded->header->contains("Route"));
}

// RFC 3261 16.5: a Request-URI in a domain this element is not responsible for is itself
// the only target. No location lookup happens, because there is no address of record to
// look up.
TEST(ProxyRouteTest, ARequestForADomainWeDoNotServeGoesToItsRequestUri) {
  Fixture f;

  f.receive(f.caller, in_dialog("OPTIONS", "sip:carol@192.0.2.20:5060", "", "z9hG4bK-options", 1));

  auto forwarded = f.request_with(f.callee_connection, "OPTIONS");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->user, "carol");
}

// RFC 3261 16.6 step 1: every branch of a fork starts from a copy of the request as it
// arrived. Forwarding the same object twice would stack this node's Via and decrement
// Max-Forwards once per attempt, so the second callee would see a different request from
// the first for no reason of the protocol's.
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
    EXPECT_EQ(attempt->header->headers_map["Record-Route"].size(), 1u);
  }

  // Two branches of one fork are two transactions (16.6 step 8).
  EXPECT_NE(forwarded[0]->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"],
            forwarded[1]->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"]);
}

// RFC 3261 16.3.4: a request carrying a Via this node wrote has been here before. The
// branch this node writes carries a hash of the fields that decide where the request
// goes, so a request that comes back unchanged is a loop and is refused.
TEST(ProxyRouteTest, ARequestThatComesBackUnchangedIs482) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  // What a looping element downstream would send back: the Via chain as it stands, one
  // more Via on top, and the Request-URI it was given by its own routing - which for a
  // loop is the address of record this node started from.
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

// RFC 3261 16.3.4: the same Via with a different Request-URI is a spiral, not a loop.
// The request is legitimately passing through this node a second time on its way
// somewhere else, and refusing it would break every service that routes that way.
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

  // It was processed as an ordinary request: nobody@example.com has no account.
  EXPECT_NE(f.response_with(f.caller_connection, 404), nullptr);
}

// RFC 3261 16.7 step 1 and 18.1.2: a response that belongs to no client transaction here
// still belongs to whoever sent the request. It goes back down the Via chain with this
// node's own Via removed and nothing remembered about it.
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

// RFC 3261 18.1.2: a response whose top Via is not one this node wrote was never this
// node's to forward, whatever it claims to answer. Forwarding it would make this node a
// relay for anything that can spell a Via.
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

// RFC 3261 16.10: a CANCEL reaching the proxy must reach the branches the proxy has
// already tried, or the callee goes on ringing after the caller has given up.
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

  // RFC 3261 9.1: a single Via, the one this node put on the INVITE it is cancelling.
  const auto& vias = sent->header->headers_map["Via"];
  ASSERT_EQ(vias.size(), 1u);
  EXPECT_EQ(vias[0]->as<ViaHeader>()->parameters["branch"], forwarded->header->headers_map["Via"][0]->as<ViaHeader>()->parameters["branch"]);
}

// RFC 3261 9.1: a CANCEL may not go out before the branch has answered provisionally,
// because until it has, the far end has no transaction by that name to cancel. The
// CANCEL waits, and the provisional response is what releases it.
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

  // Nothing has answered the branch yet, so nothing has been cancelled on it.
  EXPECT_EQ(f.request_with(f.callee_connection, "CANCEL"), nullptr);

  // The caller is told straight away regardless: it is waiting on its own transaction.
  EXPECT_NE(f.response_with(f.caller_connection, 487), nullptr);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  EXPECT_NE(f.request_with(f.callee_connection, "CANCEL"), nullptr);
}

// RFC 3261 16.10: once the caller has been told 487, the remaining bindings are no
// longer wanted. Ringing the next phone after the caller hung up is the bug this
// prevents.
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

// RFC 3261 16.7: a 503 says the next hop is out of service, which is a fact about the
// hop and not about the request. Passing it upstream would tell the caller something
// untrue about this node.
TEST(ProxyRouteTest, A503FromTheOnlyBranchBecomesA500) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(503, "Service Unavailable"));

  EXPECT_NE(f.response_with(f.caller_connection, 500), nullptr);
  EXPECT_EQ(f.response_with(f.caller_connection, 503), nullptr);
}
