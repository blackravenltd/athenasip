//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "headers/session_expires_header.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;
using athenasip::headers::SessionExpiresHeader;

namespace {

// An INVITE that asks for a session interval, with whatever the caller says about
// understanding session timers.
std::string invite_with_timer(const std::string& session_expires, const std::string& extra = "") {
  std::string raw = "INVITE sip:bob@example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: 1 INVITE\r\n";
  raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
  raw += "Max-Forwards: 70\r\n";
  if (!session_expires.empty()) raw += "Session-Expires: " + session_expires + "\r\n";
  raw += extra;
  raw += "\r\n";
  return raw;
}

SessionExpiresHeader* field_of(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
  if (!message || !message->header->contains(name)) return nullptr;
  return message->header->headers_map[name][0]->as<SessionExpiresHeader>();
}

bool has_require_timer(const std::shared_ptr<SIPMessage>& message) {
  if (!message || !message->header->contains("Require")) return false;

  for (const auto& value : message->header->headers_map["Require"]) {
    if (Util::to_lower(value->to_string()) == "timer") return true;
  }

  return false;
}

}  // namespace

// RFC 4028 section 8.1: "If the request contains a Supported header field with a value
// 'timer', the proxy MAY reject the INVITE request with a 422 (Session Interval Too
// Small) response if the session interval in the Session-Expires header field is smaller
// than the minimum interval defined by the proxy's local policy."
//
// This node keeps state for the length of a session interval, so how short that interval
// may be is its business and not only the endpoints'. Before this it read whatever the
// two ends agreed and had no say at all, and sip.session_min_se was configuration that
// nothing looked at.
TEST(ProxySessionTimerTest, AnIntervalBelowTheMinimumIsRefusedWhenTheCallerSupportsTimer) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("60", "Supported: timer\r\n"));

  auto response = f.response_with(f.caller_connection, 422);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_message, "Session Interval Too Small");

  // Section 6: "The 422 response MUST contain a Min-SE header field with the minimum
  // timer for that server."
  auto* minimum = field_of(response, "Min-SE");
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->delta_seconds, 90u);

  // And the request went no further: it was answered, not forwarded.
  EXPECT_EQ(f.request_with(f.callee_connection, "INVITE"), nullptr);
}

// The same section, for a caller that never said it understood session timers: "the proxy
// cannot usefully reject the request, as this would result in a call failure. Rather, the
// proxy SHOULD insert a Min-SE header field containing its minimum interval... The proxy
// MUST then increase the Session-Expires header field value to be equal to the value in
// the Min-SE header field."
TEST(ProxySessionTimerTest, AnIntervalBelowTheMinimumIsRaisedWhenTheCallerDoesNot) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("60"));

  EXPECT_EQ(f.response_with(f.caller_connection, 422), nullptr);

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 90u);

  auto* minimum = field_of(forwarded, "Min-SE");
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->delta_seconds, 90u);

  // "The proxy MUST NOT insert or modify the value of the 'refresher' parameter."
  EXPECT_TRUE(session->refresher.empty());
}

// "If a Min-SE header field is already present, the proxy SHOULD increase (but MUST NOT
// decrease) the value to its minimum interval", and the Session-Expires goes up to meet
// whichever value wins.
TEST(ProxySessionTimerTest, AMinSeAlreadyInTheRequestIsNeverLowered) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("60", "Min-SE: 600\r\n"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* minimum = field_of(forwarded, "Min-SE");
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->delta_seconds, 600u);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 600u);
}

// An interval this node is happy with is left exactly as it arrived. 8.1: "the proxy MUST
// NOT increase the value of the Session-Expires header field", and a proxy that has
// nothing to say must not start adding a Min-SE to a request that supports timer either -
// 8.1 forbids that outright, because it is a way of forcing an interval on the endpoints.
TEST(ProxySessionTimerTest, AnAcceptableIntervalIsForwardedUntouched) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("1800;refresher=uac", "Supported: timer\r\n"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 1800u);
  EXPECT_EQ(session->refresher, "uac");

  EXPECT_FALSE(forwarded->header->contains("Min-SE"));
}

// RFC 4028 section 8.1: "A proxy MAY insert a Session-Expires header field in the request
// before forwarding it if none was present in the request." This is what gets an expiry
// onto a call whose caller never mentioned session timers but whose callee knows what one
// is - and section 9 Table 2 is why it is safe rather than a way of breaking calls: a
// timer-aware UAS handed an interval by a UAC that does not support the extension takes
// the refreshing on itself.
TEST(ProxySessionTimerTest, ARequestWithNoIntervalIsGivenThisNodes) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer(""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 1800u);

  // "The proxy MUST NOT include a refresher parameter in the header field value."
  EXPECT_TRUE(session->refresher.empty());

  // And no Min-SE: this node did not raise anything, and 8.1 forbids adding one to a
  // request that supports timer at all.
  EXPECT_FALSE(forwarded->header->contains("Min-SE"));
}

// "not with a duration lower than the value in the Min-SE header field in the request, if
// it is present". The caller's floor wins over this node's preference.
TEST(ProxySessionTimerTest, AnInsertedIntervalRespectsTheRequestsMinSe) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("", "Min-SE: 3600\r\n"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 3600u);
}

// Zero is the switch that says do not insert, and a node set that way leaves a call that
// asked for no interval exactly as it arrived.
TEST(ProxySessionTimerTest, InsertionIsOffWhenTheNodeHasNoIntervalToOffer) {
  ProxyFixture f;
  f.config->sip_session_expires = 0;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer(""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_FALSE(forwarded->header->contains("Session-Expires"));
  EXPECT_FALSE(forwarded->header->contains("Min-SE"));
}

// RFC 4028 section 8.2: the caller asked for a session timer and the callee answered
// without one, which means the callee does not implement the extension. "Because there is
// no Session-Expires or Require header field in the response, the proxy knows that it is
// the first session-timer-aware proxy to receive the response. This proxy MUST insert a
// Session-Expires header field into the response with the value it remembered from the
// forwarded request. It MUST set the value of the 'refresher' parameter to 'uac'."
TEST(ProxySessionTimerTest, ACalleeThatIgnoresTheTimerIsAnsweredForByTheProxy) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("1800", "Supported: timer\r\n"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(200, "OK"));

  auto answer = f.response_with(f.caller_connection, 200);
  ASSERT_NE(answer, nullptr);

  auto* session = field_of(answer, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 1800u);

  // The callee cannot refresh a session it does not know it has, so the caller does.
  EXPECT_EQ(session->refresher, "uac");

  // "The proxy MUST add the 'timer' option tag to any Require header field in the
  // response, and if none was present, add the Require header field with that value."
  EXPECT_TRUE(has_require_timer(answer));

  // And the node now has an interval to watch, which is what the whole exchange was for.
  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->session_interval, 1800u);
}

// "If the received response contains a Session-Expires header field, no modification of
// the response is needed." The two ends settled it between themselves.
TEST(ProxySessionTimerTest, ACalleeThatAnswersWithATimerIsLeftAlone) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("1800", "Supported: timer\r\n"));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:bob@192.0.2.20:5060", "Session-Expires: 900;refresher=uas\r\n"));

  auto answer = f.response_with(f.caller_connection, 200);
  ASSERT_NE(answer, nullptr);

  auto* session = field_of(answer, "Session-Expires");
  ASSERT_NE(session, nullptr);

  // 8.2: "The proxy MUST NOT modify the value of the Session-Expires header field
  // received in the response."
  EXPECT_EQ(session->delta_seconds, 900u);
  EXPECT_EQ(session->refresher, "uas");
}

// A caller that never claimed to support session timers gets no invented one either: 8.2
// says that where neither end supports the extension "the proxy forwards the response
// upstream normally. There is no session expiration for this session."
TEST(ProxySessionTimerTest, NeitherEndSupportingTheTimerLeavesNoSessionExpiration) {
  ProxyFixture f;
  f.bind_bob();

  // This node offered an interval on the way through; the callee said nothing about it,
  // and neither did the caller. 8.2: "the proxy forwards the response upstream normally.
  // There is no session expiration for this session."

  f.receive(f.caller, invite_with_timer(""));
  ASSERT_NE(f.request_with(f.callee_connection, "INVITE"), nullptr);

  f.receive(f.callee, f.response_from_callee(200, "OK"));

  auto answer = f.response_with(f.caller_connection, 200);
  ASSERT_NE(answer, nullptr);

  EXPECT_FALSE(answer->header->contains("Session-Expires"));
  EXPECT_FALSE(has_require_timer(answer));

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->session_interval, 0u);
}

// RFC 4028 section 8.1: "If the request did not contain a Supported header field with the
// value 'timer', the proxy MAY insert a Require header field with the value 'timer' into
// the request. However, this is NOT RECOMMENDED."
//
// Insisting is what turns a callee that does not implement the extension from a call with
// no expiry into a call that fails with 420 Bad Extension, so it is off unless an operator
// has said they want it and knows every handset on the fleet.
TEST(ProxySessionTimerTest, RequiringTheTimerIsOffUnlessAskedFor) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer(""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  ASSERT_NE(field_of(forwarded, "Session-Expires"), nullptr);
  EXPECT_FALSE(has_require_timer(forwarded));
}

TEST(ProxySessionTimerTest, RequiringTheTimerPutsItOnTheRequest) {
  ProxyFixture f;
  f.config->sip_require_session_timer = true;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer(""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_TRUE(has_require_timer(forwarded));
}

// 8.1 puts the Require on only where the caller said nothing about session timers: "This
// header field is not needed if a Supported header field was in the request; in this case,
// the proxy would already be sure the session timer can be used for the session."
TEST(ProxySessionTimerTest, RequiringTheTimerIsSkippedWhenTheCallerAlreadySupportsIt) {
  ProxyFixture f;
  f.config->sip_require_session_timer = true;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("1800", "Supported: timer\r\n"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_FALSE(has_require_timer(forwarded));
}
