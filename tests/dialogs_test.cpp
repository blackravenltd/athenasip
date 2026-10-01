//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;
using athenasip::types::Dialog;

namespace {

using Fixture = ProxyFixture;

// A request inside the dialog, as each end sends it: its own tag in From, the other
// end's in To, and the route set it kept from the Record-Route.
std::string from_caller(const std::string& method, const std::string& branch, int cseq) {
  std::string raw = method + " sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
  raw += "Route: <sip:192.0.2.1:5060;lr>\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>;tag=bob\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: " + std::to_string(cseq) + " " + method + "\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "\r\n";
  return raw;
}

std::string from_callee(const std::string& method, const std::string& branch, int cseq) {
  std::string raw = method + " sip:alice@192.0.2.10:5060 SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=" + branch + "\r\n";
  raw += "Route: <sip:192.0.2.1:5060;lr>\r\n";
  raw += "From: <sip:bob@example.com>;tag=bob\r\n";
  raw += "To: <sip:alice@example.com>;tag=alice\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: " + std::to_string(cseq) + " " + method + "\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "\r\n";
  return raw;
}

std::string cancel_from_caller() {
  std::string raw = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:bob@example.com>\r\n";
  raw += "Call-ID: call-proxy\r\n";
  raw += "CSeq: 1 CANCEL\r\n";
  raw += "\r\n";
  return raw;
}

// Brings a call up: INVITE, 180, 200.
void answer(Fixture& f) {
  f.bind_bob();
  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  f.receive(f.callee, f.response_from_callee(200, "OK"));
}

}  // namespace

// RFC 3261 12.1: a 2xx to an INVITE establishes the dialog, and 12.1.1 gives it its
// identifier - the Call-ID and the tag from each end.
TEST(DialogsTest, A2xxEstablishesTheDialog) {
  Fixture f;
  answer(f);

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);

  EXPECT_EQ(dialog->state, Dialog::State::Confirmed);
  EXPECT_EQ(dialog->call_id, "call-proxy");
  EXPECT_EQ(dialog->caller_tag, "alice");
  EXPECT_EQ(dialog->callee_tag, "bob");
  EXPECT_EQ(dialog->id(), "call-proxy|alice|bob");
}

// RFC 3261 12.1.1: the remote target is the Contact the far end offered. Each end has
// one, and they are not interchangeable - a request to the callee that went to the
// caller's Contact would arrive at the wrong phone.
TEST(DialogsTest, EachEndsTargetIsItsOwnContact) {
  Fixture f;
  answer(f);

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);

  ASSERT_NE(dialog->caller_target, nullptr);
  EXPECT_EQ(dialog->caller_target->host, "192.0.2.10");

  ASSERT_NE(dialog->callee_target, nullptr);
  EXPECT_EQ(dialog->callee_target->host, "192.0.2.20");
}

// RFC 3261 12.1.1: the route set is the Record-Route of the response. This node put
// itself in it on the way out, which is the reason the BYE comes back through here.
//
// Twice, because it record-routes both interfaces (RFC 5658): one value for the flow
// the request arrived on and one for the flow it left on. Both name this node.
TEST(DialogsTest, TheRouteSetIsWhatThisNodeRecorded) {
  Fixture f;
  answer(f);

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);

  ASSERT_EQ(dialog->route_set.size(), 2u);

  for (const auto& route : dialog->route_set) {
    EXPECT_EQ(route->host, "192.0.2.1");
    EXPECT_TRUE(route->has_parameter("lr"));

    // The flow token, which is what an in-dialog request is routed by once the route
    // set is spent (RFC 5626 section 5.1).
    EXPECT_FALSE(route->user.empty());
  }
}

// RFC 3261 12.1: a provisional response carrying a To tag creates an early dialog, and
// the 2xx that follows confirms that same dialog. Treating the 2xx as a second one would
// leave the early one behind for ever, with nothing to end it.
TEST(DialogsTest, AProvisionalCreatesTheEarlyDialogAndThe2xxConfirmsIt) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));

  auto early = f.only_dialog();
  ASSERT_NE(early, nullptr);
  EXPECT_EQ(early->state, Dialog::State::Early);
  EXPECT_EQ(early->callee_tag, "bob");

  f.receive(f.callee, f.response_from_callee(200, "OK"));

  EXPECT_EQ(f.dialogs().size(), 1u);
  EXPECT_EQ(early->state, Dialog::State::Confirmed);
}

// RFC 3261 12.2.2: a request belongs to a dialog when the Call-ID and both tags match.
// A proxy sees both ends, so the tags arrive one way round from the caller and the other
// way round from the callee, and both are the same dialog.
TEST(DialogsTest, AnInDialogRequestIsMatchedFromEitherEnd) {
  Fixture f;
  answer(f);

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);

  EXPECT_TRUE(dialog->matches("call-proxy", "alice", "bob"));
  EXPECT_TRUE(dialog->matches("call-proxy", "bob", "alice"));

  EXPECT_FALSE(dialog->matches("call-proxy", "alice", "carol"));
  EXPECT_FALSE(dialog->matches("another-call", "alice", "bob"));
}

// RFC 3261 12.2.1.1: each end has its own sequence. A re-INVITE from the callee advances
// the callee's, and leaves the caller's where it was.
TEST(DialogsTest, EachEndHasItsOwnSequenceNumber) {
  Fixture f;
  answer(f);

  f.receive(f.caller, from_caller("INVITE", "z9hG4bK-reinvite", 2));
  f.receive(f.callee, from_callee("INVITE", "z9hG4bK-callee-reinvite", 7));

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);

  EXPECT_EQ(dialog->caller_cseq, 2u);
  EXPECT_EQ(dialog->callee_cseq, 7u);
}

// RFC 3261 12.2.1.1: a re-INVITE may move the remote target, and a node that anchors
// media has to follow it or send media where the phone no longer is.
TEST(DialogsTest, AReInviteMovesTheRemoteTarget) {
  Fixture f;
  answer(f);

  std::string reinvite = "INVITE sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
  reinvite += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-reinvite\r\n";
  reinvite += "Route: <sip:192.0.2.1:5060;lr>\r\n";
  reinvite += "From: <sip:alice@example.com>;tag=alice\r\n";
  reinvite += "To: <sip:bob@example.com>;tag=bob\r\n";
  reinvite += "Call-ID: call-proxy\r\n";
  reinvite += "CSeq: 2 INVITE\r\n";
  reinvite += "Contact: <sip:alice@192.0.2.10:5080>\r\n";
  reinvite += "Max-Forwards: 70\r\n";
  reinvite += "\r\n";

  f.receive(f.caller, reinvite);

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  ASSERT_NE(dialog->caller_target, nullptr);
  EXPECT_EQ(dialog->caller_target->port.value_or(0), 5080);
}

// RFC 3261 15.1: a BYE ends the dialog, from whichever end it comes. This is what the
// node tracks dialogs for - without it nothing tells it the call is over.
TEST(DialogsTest, AByeEndsTheDialog) {
  Fixture f;
  answer(f);
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.receive(f.caller, from_caller("BYE", "z9hG4bK-bye", 2));

  EXPECT_EQ(f.dialogs().size(), 0u);
}

TEST(DialogsTest, AByeFromTheCalleeEndsTheDialogToo) {
  Fixture f;
  answer(f);
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.receive(f.callee, from_callee("BYE", "z9hG4bK-callee-bye", 5));

  EXPECT_EQ(f.dialogs().size(), 0u);
}

// An INVITE that is never answered leaves nothing behind. A record kept for a call that
// did not happen is a leak, and on a busy node it is the only kind that matters.
TEST(DialogsTest, AnInviteThatFailsLeavesNothingBehind) {
  Fixture f;

  f.receive(f.caller, f.invite("z9hG4bK-invite", "sip:nobody@example.com"));

  ASSERT_NE(f.response_with(f.caller_connection, 404), nullptr);
  EXPECT_EQ(f.dialogs().size(), 0u);
}

TEST(DialogsTest, ABusyCalleeLeavesNothingBehind) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(486, "Busy Here"));

  EXPECT_EQ(f.dialogs().size(), 0u);
}

// RFC 3261 9.1: a CANCEL ends the attempt it names. It carries the INVITE's To, which
// has no tag, so it has to find the call by the caller's tag alone - and it has to do so
// after a 180 has already given the callee a tag.
TEST(DialogsTest, ACancelEndsTheAttemptAfterRinging) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.receive(f.caller, cancel_from_caller());

  EXPECT_EQ(f.dialogs().size(), 0u);
}

// RFC 3261 9.1: a CANCEL has no effect on a request already answered finally. This is
// the race where the callee picked up as the caller gave up: the call is up, and only a
// BYE ends it now.
TEST(DialogsTest, ACancelAfterTheCallIsUpDoesNotEndIt) {
  Fixture f;
  answer(f);

  f.receive(f.caller, cancel_from_caller());

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->state, Dialog::State::Confirmed);
}

// RFC 3261 12.1.1: a dialog is secure when the request was sent over TLS *and* the
// Request-URI was a sips URI. Either alone is not enough - TLS on the hop this node saw
// says nothing about the hops it did not.
TEST(DialogsTest, ADialogIsSecureOnlyWithSipsOverTls) {
  auto secure_call = [](const std::string& scheme, const std::string& transport) {
    Fixture f;

    std::shared_ptr<MockConnection> uac_connection;
    auto uac = f.make_channel("192.0.2.10", &uac_connection, transport);
    f.on_strand([&uac]() { uac->authenticated_as("sip:alice@example.com"); });

    std::shared_ptr<MockConnection> uas_connection;
    auto uas = f.make_channel("192.0.2.30", &uas_connection, transport);

    std::string raw = "INVITE " + scheme + ":bob@192.0.2.30:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/" + Util::to_upper(transport) + " 192.0.2.10:5060;branch=z9hG4bK-secure\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <" + scheme + ":bob@192.0.2.30:5060>\r\n";
    raw += "Call-ID: call-secure\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";

    f.receive(uac, raw);

    auto dialogs = f.dialogs();
    return dialogs.size() == 1 && dialogs.front()->secure;
  };

  EXPECT_TRUE(secure_call("sips", "tls"));
  EXPECT_FALSE(secure_call("sips", "udp"));
  EXPECT_FALSE(secure_call("sip", "tls"));
}

// The call record is the application object hanging off the dialog, and it is what the
// admin API lists. It has to follow the dialog through every state, or a live-calls page
// shows calls that ended an hour ago.
TEST(DialogsTest, TheCallRecordFollowsTheDialog) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));

  auto call = f.call();
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->state, Call::State::Trying);
  ASSERT_EQ(call->participants.size(), 2u);
  EXPECT_TRUE(call->participants[0].originator);
  EXPECT_FALSE(call->participants[1].originator);

  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  EXPECT_EQ(call->state, Call::State::Ringing);

  f.receive(f.callee, f.response_from_callee(200, "OK"));
  EXPECT_EQ(call->state, Call::State::Connected);
  EXPECT_NE(call->answered_at, 0);

  // Both legs of a proxied two-party call are ends of the one dialog (12.1).
  ASSERT_NE(call->participants[0].dialog, nullptr);
  EXPECT_EQ(call->participants[0].dialog, call->participants[1].dialog);

  f.receive(f.caller, from_caller("BYE", "z9hG4bK-bye", 2));

  EXPECT_EQ(call->state, Call::State::Closed);
  EXPECT_NE(call->ended_at, 0);

  // And it is no longer a live call.
  EXPECT_EQ(f.call(), nullptr);
}

// RFC 4028 section 7.1: the 2xx carries what the two ends settled on. What the INVITE
// asked for is a request and the UAS may lower it, so the answer is the only value worth
// recording.
TEST(DialogsTest, TheNegotiatedSessionIntervalComesFromThe2xx) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:bob@192.0.2.20:5060", "Session-Expires: 1800;refresher=uas\r\n"));

  auto dialog = f.only_dialog();
  ASSERT_NE(dialog, nullptr);
  EXPECT_EQ(dialog->session_interval, 1800u);
  EXPECT_EQ(dialog->refresher, "uas");
}

// RFC 4028 section 8: a proxy keeping session state uses the timer to decide when to
// discard it. A call that is never refreshed is one this node has no evidence is still
// there, and holding it open for ever is the leak session timers exist to stop.
TEST(DialogsTest, AnUnrefreshedSessionLapses) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:bob@192.0.2.20:5060", "Session-Expires: 1800;refresher=uas\r\n"));
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.on_strand([&f]() { f.timers->advance(std::chrono::seconds(1799)); });
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.on_strand([&f]() { f.timers->advance(std::chrono::seconds(2)); });
  EXPECT_EQ(f.dialogs().size(), 0u);
}

// RFC 4028 section 7: a re-INVITE or an UPDATE inside the dialog refreshes it. A request
// travelling between the two ends is both of them still being there, which is the whole
// of what a proxy needs to know.
TEST(DialogsTest, ARefreshInsideTheDialogPostponesTheLapse) {
  Fixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite("z9hG4bK-invite"));
  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:bob@192.0.2.20:5060", "Session-Expires: 1800;refresher=uac\r\n"));

  f.on_strand([&f]() { f.timers->advance(std::chrono::seconds(1700)); });
  f.receive(f.caller, from_caller("UPDATE", "z9hG4bK-refresh", 2));

  // The old deadline passes and the call is still up, because the refresh moved it.
  f.on_strand([&f]() { f.timers->advance(std::chrono::seconds(200)); });
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.on_strand([&f]() { f.timers->advance(std::chrono::seconds(1700)); });
  EXPECT_EQ(f.dialogs().size(), 0u);
}

// A call whose ends never agreed a session timer must not be timed out. Nothing said how
// long it should last, so a node that ended it would be ending a call that is still up.
TEST(DialogsTest, ACallWithNoSessionTimerNeverLapses) {
  Fixture f;
  answer(f);

  f.on_strand([&f]() { f.timers->advance(std::chrono::hours(24)); });

  EXPECT_EQ(f.dialogs().size(), 1u);
}
