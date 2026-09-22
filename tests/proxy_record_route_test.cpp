//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "headers/sip_identity_header.h"
#include "helpers/proxy_fixture_helper.h"
#include "types/sip_uri.h"

using namespace athenasip;

namespace {

// Bob is a browser: he reached the node over a secure WebSocket and the Contact he
// offered resolves to nothing at all, which is true of every browser. Alice is a desk
// phone on UDP. This is the call Milestone 3 exists for, and the one that shows what
// Record-Route has to carry.
struct MixedTransportFixture : ProxyFixture {
  static constexpr const char* kBrowserContact = "sip:bob@df7jal23ls0d.invalid;transport=wss";

  MixedTransportFixture() {
    callee = make_channel("192.0.2.20", &callee_connection, "wss", 443);
    register_binding(bob, std::make_shared<types::SIPUri>(kBrowserContact), callee, 3600);
  }

  // The route set as the caller builds it: the response's Record-Route values, reversed
  // (RFC 3261 12.1.2).
  std::vector<std::string> caller_route_set() {
    auto forwarded = request_with(callee_connection, "INVITE");
    if (!forwarded) return {};

    std::vector<std::string> routes;
    for (const auto& value : forwarded->header->headers_map["Record-Route"]) routes.push_back(value->to_string());

    std::reverse(routes.begin(), routes.end());
    return routes;
  }

  // And as the callee builds it: the request's Record-Route values, in order
  // (RFC 3261 12.1.1).
  std::vector<std::string> callee_route_set() {
    auto forwarded = request_with(callee_connection, "INVITE");
    if (!forwarded) return {};

    std::vector<std::string> routes;
    for (const auto& value : forwarded->header->headers_map["Record-Route"]) routes.push_back(value->to_string());

    return routes;
  }

  static std::string bye(const std::string& from, const std::string& from_tag, const std::string& to, const std::string& to_tag, const std::string& request_uri,
                         const std::vector<std::string>& routes) {
    std::string raw = "BYE " + request_uri + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-bye\r\n";
    for (const auto& route : routes) raw += "Route: " + route + "\r\n";
    raw += "From: <" + from + ">;tag=" + from_tag + "\r\n";
    raw += "To: <" + to + ">;tag=" + to_tag + "\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 2 BYE\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }

  void answer() {
    receive(caller, invite());
    receive(callee, response_from_callee(200, "OK", "bob", kBrowserContact));
    settle();
  }
};

}  // namespace

// RFC 5658 section 3: the topmost value names the interface the request is sent on and
// the second the interface it arrived on. That order is what gives each end the value
// facing it, because a UAS reads the route set from the request in order and a UAC
// reads it from the response reversed.
TEST(ProxyRecordRouteTest, TwoValuesAreWrittenOutboundFirst) {
  MixedTransportFixture f;

  f.receive(f.caller, f.invite());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  const auto& routes = forwarded->header->headers_map["Record-Route"];
  ASSERT_EQ(routes.size(), 2u);

  auto outbound = std::make_shared<types::SIPUri>(routes[0]->as<athenasip::headers::SIPIdentityHeader>()->value->uri->to_string());
  auto inbound = std::make_shared<types::SIPUri>(routes[1]->as<athenasip::headers::SIPIdentityHeader>()->value->uri->to_string());

  // Sent on the browser's secure WebSocket, arrived on the phone's UDP.
  EXPECT_EQ(outbound->parameter("transport"), "wss");
  EXPECT_FALSE(inbound->has_parameter("transport"));

  // Both are this node, and both are loose routers.
  EXPECT_EQ(outbound->host, "192.0.2.1");
  EXPECT_EQ(inbound->host, "192.0.2.1");
  EXPECT_TRUE(outbound->has_parameter("lr"));
  EXPECT_TRUE(inbound->has_parameter("lr"));
}

// The token is what makes the pair worth writing, and it must not be the flow's name:
// the flow id is the far end's address, and a route set that spelled it out would hand
// one end of an anchored call the other end's address.
TEST(ProxyRecordRouteTest, EachValueCarriesAnOpaqueTokenForTheSideItFaces) {
  MixedTransportFixture f;

  f.receive(f.caller, f.invite());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  const auto& routes = forwarded->header->headers_map["Record-Route"];
  ASSERT_EQ(routes.size(), 2u);

  const auto outbound = routes[0]->as<athenasip::headers::SIPIdentityHeader>()->value->uri->user;
  const auto inbound = routes[1]->as<athenasip::headers::SIPIdentityHeader>()->value->uri->user;

  EXPECT_FALSE(outbound.empty());
  EXPECT_FALSE(inbound.empty());
  EXPECT_NE(outbound, inbound);

  // Neither says where anybody is.
  for (const auto& token : {outbound, inbound}) {
    EXPECT_EQ(token.find("192.0.2."), std::string::npos) << token;
    EXPECT_EQ(token.find("wss"), std::string::npos) << token;
    EXPECT_EQ(token.find("udp"), std::string::npos) << token;
  }
}

// The whole point. Bob is a browser, so his Contact resolves to nothing: a BYE
// forwarded to it would go nowhere, and a call that cannot be hung up is worse than
// one that cannot be made. The flow token in the route set is the only way back.
TEST(ProxyRecordRouteTest, AByeReachesABrowserWhoseContactResolvesToNothing) {
  MixedTransportFixture f;
  f.answer();

  const auto before = ProxyFixture::requests_with(f.callee_connection, "BYE").size();

  f.receive(f.caller, MixedTransportFixture::bye("sip:alice@example.com", "alice", "sip:bob@example.com", "bob", MixedTransportFixture::kBrowserContact,
                                                 f.caller_route_set()));

  auto delivered = ProxyFixture::requests_with(f.callee_connection, "BYE");
  ASSERT_EQ(delivered.size(), before + 1);

  // Forwarded as it was addressed: the remote target is still the Contact, and only
  // the flow it went down was decided by the token (RFC 3261 16.6 leaves the
  // Request-URI alone when a route set decided the hop).
  EXPECT_EQ(delivered.back()->header->request_uri->host, "df7jal23ls0d.invalid");
}

// And the other way. Bob's route set is the request's values in order, so the one he
// sends to is the outbound value and the one behind it is the inbound: the token that
// comes out names Alice's flow.
TEST(ProxyRecordRouteTest, AByeFromTheBrowserReachesTheCallerByItsOwnToken) {
  MixedTransportFixture f;
  f.answer();

  const auto before = ProxyFixture::requests_with(f.caller_connection, "BYE").size();

  f.receive(f.callee,
            MixedTransportFixture::bye("sip:bob@example.com", "bob", "sip:alice@example.com", "alice", "sip:alice@192.0.2.10:5060", f.callee_route_set()));

  EXPECT_EQ(ProxyFixture::requests_with(f.caller_connection, "BYE").size(), before + 1);
}

// RFC 5658 section 3.2: both values name this node, and a proxy that removed only the
// first would forward the request straight back to itself.
TEST(ProxyRecordRouteTest, BothOfThisNodesRoutesAreRemoved) {
  MixedTransportFixture f;
  f.answer();

  f.receive(f.caller, MixedTransportFixture::bye("sip:alice@example.com", "alice", "sip:bob@example.com", "bob", MixedTransportFixture::kBrowserContact,
                                                 f.caller_route_set()));

  auto delivered = ProxyFixture::requests_with(f.callee_connection, "BYE");
  ASSERT_FALSE(delivered.empty());

  EXPECT_FALSE(delivered.back()->header->contains("Route")) << "a Route naming this node was left in the request";
}

// A token naming a flow that has since closed names nothing, and the Contact is all
// that is left to go on. For a desk phone that is the right answer; for a browser the
// attempt fails, which is what a dialog whose flow has gone deserves.
TEST(ProxyRecordRouteTest, AClosedFlowFallsBackToTheContact) {
  ProxyFixture f;
  f.bind_bob("sip:bob@192.0.2.20:5060");

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.settle();

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  std::vector<std::string> routes;
  for (const auto& value : forwarded->header->headers_map["Record-Route"]) routes.push_back(value->to_string());
  std::reverse(routes.begin(), routes.end());

  // The browser hangs up and its socket goes with it.
  f.on_strand([&f]() { f.callee->close(); });
  f.settle();

  const auto before = ProxyFixture::requests_with(f.callee_connection, "BYE").size();

  f.receive(f.caller, MixedTransportFixture::bye("sip:alice@example.com", "alice", "sip:bob@example.com", "bob", "sip:bob@192.0.2.20:5060", routes));

  // Nothing crashed, and the node did not forward the request to itself.
  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "BYE").size(), before);
}
