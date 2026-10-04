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

// An INVITE asking for a session interval, plus any extra header lines.
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

// RFC 4028 8.1: when the caller supports timer, an interval below sip.session_min_se is
// refused with 422.
TEST(ProxySessionTimerTest, AnIntervalBelowTheMinimumIsRefusedWhenTheCallerSupportsTimer) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("60", "Supported: timer\r\n"));

  auto response = f.response_with(f.caller_connection, 422);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_message, "Session Interval Too Small");

  // RFC 4028 6: the 422 carries this node's Min-SE.
  auto* minimum = field_of(response, "Min-SE");
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->delta_seconds, 90u);

  // Answered, not forwarded.
  EXPECT_EQ(f.request_with(f.callee_connection, "INVITE"), nullptr);
}

// RFC 4028 8.1: a caller without timer support cannot usefully be refused. The proxy
// inserts Min-SE and raises Session-Expires to match it.
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

  // RFC 4028 8.1: the proxy does not insert or modify the refresher parameter.
  EXPECT_TRUE(session->refresher.empty());
}

// RFC 4028 8.1: a Min-SE already present is raised, never lowered, and Session-Expires
// rises to meet whichever value wins.
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

// RFC 4028 8.1: an acceptable interval is forwarded untouched. The proxy neither raises
// Session-Expires nor adds Min-SE to a request that supports timer.
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

// RFC 4028 8.1: a request with no Session-Expires is given this node's. Section 9 Table
// 2 makes that safe: a timer-aware UAS facing a UAC without the extension refreshes itself.
TEST(ProxySessionTimerTest, ARequestWithNoIntervalIsGivenThisNodes) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer(""));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto* session = field_of(forwarded, "Session-Expires");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->delta_seconds, 1800u);

  // RFC 4028 8.1: an inserted Session-Expires carries no refresher parameter.
  EXPECT_TRUE(session->refresher.empty());

  // No Min-SE: 8.1 forbids adding one to a request that supports timer.
  EXPECT_FALSE(forwarded->header->contains("Min-SE"));
}

// RFC 4028 8.1: an inserted interval is not below the request's Min-SE.
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

// A configured interval of zero turns insertion off.
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

// RFC 4028 8.2: when the caller asked for a timer and the callee answered without one,
// the proxy inserts the Session-Expires it remembered, with refresher=uac.
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

  // The callee does not know of the session timer, so the caller refreshes.
  EXPECT_EQ(session->refresher, "uac");

  // RFC 4028 8.2: the proxy adds timer to the response's Require.
  EXPECT_TRUE(has_require_timer(answer));

  // The dialog now has an interval to watch.
  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->session_interval, 1800u);
}

// RFC 4028 8.2: a response that already carries Session-Expires is left alone.
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

  // RFC 4028 8.2: the proxy does not modify the Session-Expires in the response.
  EXPECT_EQ(session->delta_seconds, 900u);
  EXPECT_EQ(session->refresher, "uas");
}

// RFC 4028 8.2: where neither end supports timer the response is forwarded normally and
// the session has no expiration.
TEST(ProxySessionTimerTest, NeitherEndSupportingTheTimerLeavesNoSessionExpiration) {
  ProxyFixture f;
  f.bind_bob();

  // This node offered an interval; neither end took it up.

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

// RFC 4028 8.1: inserting Require: timer is NOT RECOMMENDED, since a callee without the
// extension then fails the call with 420. It is off unless configured.
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

// RFC 4028 8.1: Require: timer is not added when the caller already supports timer.
TEST(ProxySessionTimerTest, RequiringTheTimerIsSkippedWhenTheCallerAlreadySupportsIt) {
  ProxyFixture f;
  f.config->sip_require_session_timer = true;
  f.bind_bob();

  f.receive(f.caller, invite_with_timer("1800", "Supported: timer\r\n"));

  auto forwarded = f.request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_FALSE(has_require_timer(forwarded));
}
