//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../helpers/proxy_fixture_helper.h"
#include "media/media_engine.h"
#include "qualifier.h"

using namespace athenasip;
using athenasip::media::Flags;

namespace {

// An engine that takes everything, changes nothing, and remembers what it was told. The
// question these tests ask is what the proxy says to an engine, not what an engine does
// with it, and a real engine would answer both at once.
class RecordingMediaEngine : public media::MediaEngine {
 public:
  struct Call {
    bool was_offer = false;
    Flags flags;
  };

  std::string name() const override { return "recording"; }
  std::string version() const override { return "0.0.1"; }

  void connect(plugins::Executor on, plugins::StatusHandler handler) override {
    _connected = true;
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void close() override { _connected = false; }
  bool is_connected() const override { return _connected; }

  media::Capabilities capabilities() const override {
    media::Capabilities capabilities;
    capabilities.bridge = true;
    return capabilities;
  }

  // An engine that relays plain RTP and converts nothing, which is what builtin is.
  void plain_rtp_only_set() { _plain_rtp_only = true; }
  bool produces(media::Profile profile) const override { return !_plain_rtp_only || profile == media::Profile::PlainRtp || profile == media::Profile::Mirror; }

  void offer(plugins::Executor on, std::shared_ptr<athenasip::Call> call, std::string sdp, Flags flags, MediaHandler handler) override {
    (void)call;
    _record(true, flags);
    _complete(std::move(on), std::move(handler), media::Result::success(std::move(sdp)));
  }

  void answer(plugins::Executor on, std::shared_ptr<athenasip::Call> call, std::string sdp, Flags flags, MediaHandler handler) override {
    (void)call;
    _record(false, flags);
    _complete(std::move(on), std::move(handler), media::Result::success(std::move(sdp)));
  }

  void release(plugins::Executor on, std::shared_ptr<athenasip::Call> call, plugins::StatusHandler handler) override {
    (void)call;
    _releases++;
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void query(plugins::Executor on, std::shared_ptr<athenasip::Call> call, plugins::Handler<std::string> handler) override {
    (void)call;
    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success("{}"));
  }

  int releases() const { return _releases; }

  std::vector<Call> calls() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _calls;
  }

 private:
  void _record(bool was_offer, const Flags& flags) {
    std::lock_guard<std::mutex> lock(_mutex);
    _calls.push_back(Call{was_offer, flags});
  }

  mutable std::mutex _mutex;
  std::vector<Call> _calls;
  std::atomic<int> _releases{0};
  bool _connected = false;
  bool _plain_rtp_only = false;
};

const std::string kOffer =
    "v=0\r\n"
    "o=alice 1 1 IN IP4 192.0.2.10\r\n"
    "s=-\r\n"
    "c=IN IP4 192.0.2.10\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0\r\n";

// What a browser writes, and what AthenaPhone writes over any transport it signals on:
// ICE credentials, a DTLS fingerprint and the SAVPF profile (RFC 8839, 8122, 5764).
const std::string kWebRtcOffer =
    "v=0\r\n"
    "o=- 1 1 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/rEckDvYgxFs9WU3wMcYY\r\n"
    "a=fingerprint:sha-256 AA:BB:CC\r\n"
    "a=rtcp-mux\r\n";

// The desk phone that wants its media encrypted and has never heard of DTLS: RTP/SAVP
// with the keys in the description (RFC 4568).
const std::string kSdesOffer =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.0.2.30\r\n"
    "s=-\r\n"
    "c=IN IP4 192.0.2.30\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/SAVP 0\r\n"
    "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR\r\n";

struct ProfileFixture : ProxyFixture {
  std::shared_ptr<RecordingMediaEngine> engine = std::make_shared<RecordingMediaEngine>();

  // Each end's transport, which is what a node has to go on until that end has
  // described itself, and nothing after that.
  explicit ProfileFixture(const std::string& callee_transport, const std::string& caller_transport = "udp") {
    engine->connect(core->strand(), [](plugins::Status) {});
    core->media_register(engine);
    settle();

    // What these tests are about is the rule that reads a leg that has said nothing from
    // its transport, which since 2026-10-01 is a setting rather than the default.
    config->behaviour.profiles = types::MediaPolicy::Profiles::FromTransport;

    caller = make_channel("192.0.2.10", &caller_connection, caller_transport);

    callee = make_channel("192.0.2.20", &callee_connection, callee_transport);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), callee, 3600);

    // Both registered over the connections they call on, as ProxyFixture's own pair have.
    on_strand([this]() {
      caller->authenticated_as("sip:alice@example.com");
      callee->authenticated_as("sip:bob@example.com");
    });
  }

  static std::string with_body(std::string raw, const std::string& sdp) {
    if (raw.empty()) return raw;
    raw.insert(raw.size() - 2, "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n");
    return raw + sdp;
  }

  std::string invite_with_body(const std::string& sdp = kOffer) { return with_body(invite(), sdp); }

  std::string ok_with(const std::string& sdp) { return with_body(response_from_callee(200, "OK"), sdp); }

  // The route set this node wrote into the INVITE, as the leg that learned it sends it
  // back. A UAS takes Record-Route in the order it arrived (RFC 3261 12.1.1) and a UAC
  // takes it reversed (12.1.2).
  std::string routes(bool reversed) {
    auto forwarded = request_with(callee_connection, "INVITE");
    if (!forwarded) return "";

    std::vector<std::string> values;
    for (const auto& value : forwarded->header->headers_map["Record-Route"]) values.push_back(value->to_string());
    if (reversed) std::reverse(values.begin(), values.end());

    std::string out;
    for (const auto& value : values) out += "Route: " + value + "\r\n";
    return out;
  }

  // A final response to the INVITE most recently forwarded to the callee, which after a
  // re-offer is not the first one.
  std::string response_to_latest(int code, const std::string& reason, const std::string& sdp = "", std::shared_ptr<MockConnection> connection = nullptr,
                                 const std::string& to_tag = "bob") {
    const auto forwarded = requests_with(connection ? connection : callee_connection, "INVITE");
    if (forwarded.empty()) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded.back()->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    for (const auto& route : forwarded.back()->header->headers_map["Record-Route"]) raw += "Record-Route: " + route->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=" + to_tag + "\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    if (code < 300) raw += "Contact: <sip:bob@192.0.2.20:5060>\r\n";
    raw += "\r\n";
    return sdp.empty() ? raw : with_body(raw, sdp);
  }

  std::vector<media::Reoffer> reoffers() {
    return on_strand([this]() { return core->reoffers().list(); });
  }

  // Hold, resume, or any other mid-call re-offer, sent by the end that answered.
  std::string reinvite_from_callee(const std::string& sdp) {
    std::string raw = "INVITE sip:alice@192.0.2.10:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=z9hG4bK-reinvite\r\n";
    raw += routes(false);
    raw += "From: <sip:bob@example.com>;tag=bob\r\n";
    raw += "To: <sip:alice@example.com>;tag=alice\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 2 INVITE\r\n";
    raw += "Contact: <sip:bob@192.0.2.20:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return with_body(raw, sdp);
  }

  // The caller's ACK, which carries the answer when the INVITE carried no offer.
  std::string ack_with(const std::string& sdp) {
    std::string raw = "ACK sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-ack\r\n";
    raw += routes(true);
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=bob\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 ACK\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return with_body(raw, sdp);
  }
};

}  // namespace

// The transport is the only thing that says what the far leg is before that leg has
// described itself, and a browser reaches a node over a WebSocket and nothing else
// (RFC 7118).
TEST(MediaProfileTest, TheProfileOfALegComesFromItsTransport) {
  EXPECT_EQ(Flags::profile_for_transport("ws"), Flags::Profile::WebRtc);
  EXPECT_EQ(Flags::profile_for_transport("wss"), Flags::Profile::WebRtc);
  EXPECT_EQ(Flags::profile_for_transport("WSS"), Flags::Profile::WebRtc);

  EXPECT_EQ(Flags::profile_for_transport("udp"), Flags::Profile::PlainRtp);
  EXPECT_EQ(Flags::profile_for_transport("tcp"), Flags::Profile::PlainRtp);
  EXPECT_EQ(Flags::profile_for_transport("tls"), Flags::Profile::PlainRtp);

  // Nothing known is not the same as plain RTP, and the engine is left to decide.
  EXPECT_EQ(Flags::profile_for_transport(""), Flags::Profile::Mirror);
}

// And a description says what the end that wrote it is, which is better than the
// transport because it is not a guess. DTLS is the WebRTC handshake and nothing else
// uses it; keys in the description are SDES; neither is plain RTP.
TEST(MediaProfileTest, ADescriptionSaysWhatTheEndThatWroteItIs) {
  EXPECT_EQ(Flags::from_sdp(kWebRtcOffer).stated(), Flags::Profile::WebRtc);
  EXPECT_EQ(Flags::from_sdp(kSdesOffer).stated(), Flags::Profile::SrtpSdes);
  EXPECT_EQ(Flags::from_sdp(kOffer).stated(), Flags::Profile::PlainRtp);

  // RFC 5764 section 8: UDP/TLS/RTP/SAVP and SAVPF are DTLS-SRTP by name. The profile on
  // the m-line is the statement, so a capability description - an OPTIONS 200 that has no
  // DTLS session to fingerprint yet - says WebRTC without one.
  const std::string profile_only =
      "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:111 opus/48000/2\r\n";
  EXPECT_EQ(Flags::from_sdp(profile_only).stated(), Flags::Profile::WebRtc);

  // Nothing readable says nothing, which is not the same as saying plain RTP. Recording
  // a guess would have the leg quoted on something it never said.
  EXPECT_FALSE(Flags::from_sdp("not a session description").stated().has_value());
}

// The offer goes to Bob, so what the engine has to produce is what Bob's leg needs.
TEST(MediaProfileTest, AnOfferForwardedToABrowserAsksForTheWebRtcProfile) {
  ProfileFixture f("wss");

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());

  EXPECT_TRUE(calls[0].was_offer);
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::WebRtc);

  // What the offer itself was is a separate question, and it was plain RTP.
  EXPECT_FALSE(calls[0].flags.ice);
  EXPECT_FALSE(calls[0].flags.dtls);
}

TEST(MediaProfileTest, AnOfferForwardedToAPhoneAsksForPlainRtp) {
  ProfileFixture f("udp");

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());

  EXPECT_EQ(calls[0].flags.target, Flags::Profile::PlainRtp);
}

// The answer travels the other way, so the leg that decides is the one the request
// came in on. Reading the wrong end here is how a browser ends up being handed an
// answer with no ICE.
TEST(MediaProfileTest, AnAnswerTakesTheProfileOfTheEndItGoesBackTo) {
  ProfileFixture f("udp", "wss");

  f.receive(f.caller, f.invite_with_body(kWebRtcOffer));
  f.receive(f.callee, f.ok_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);

  const auto& answer = calls.back();
  EXPECT_FALSE(answer.was_offer);
  EXPECT_EQ(answer.flags.target, Flags::Profile::WebRtc);
}

// RFC 3264: an answer goes back to the end that made the offer, and that offer already
// says exactly what that end asked for. Reading its transport instead is how an
// AthenaPhone on UDP - WebRTC media over ordinary SIP signalling - is handed an answer
// it cannot use.
TEST(MediaProfileTest, AnAnswerIsProfiledFromTheOfferItAnswers) {
  ProfileFixture f("udp", "udp");

  f.receive(f.caller, f.invite_with_body(kWebRtcOffer));
  f.receive(f.callee, f.ok_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);

  const auto& answer = calls.back();
  EXPECT_FALSE(answer.was_offer);
  EXPECT_EQ(answer.flags.target, Flags::Profile::WebRtc);
}

// And what a leg said is remembered for the rest of the call. Hold and resume are
// re-INVITEs, and profiling one from the transport would have this node contradict
// mid-call what it produced for the same leg a moment earlier.
TEST(MediaProfileTest, ALegThatHasDescribedItselfIsNotReadFromItsTransportAgain) {
  ProfileFixture f("wss", "udp");

  f.receive(f.caller, f.invite_with_body(kWebRtcOffer));
  f.receive(f.callee, f.ok_with(kWebRtcOffer));

  const auto before = f.engine->calls().size();
  ASSERT_GE(before, 2u);

  f.receive(f.callee, f.reinvite_from_callee(kWebRtcOffer));

  const auto calls = f.engine->calls();
  ASSERT_GT(calls.size(), before);

  // Alice signals over UDP and her media is WebRTC, which only her own offer says.
  EXPECT_TRUE(calls.back().was_offer);
  EXPECT_EQ(calls.back().flags.target, Flags::Profile::WebRtc);
}

// A leg this node has never heard from is the one case the transport and the realm are
// for, and the answer going back to the end that offered is not it.
TEST(MediaProfileTest, TheFirstOfferTowardsALegIsStillReadFromItsTransport) {
  ProfileFixture f("wss", "udp");

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::WebRtc);
}

// RFC 3264 section 5 inverts the exchange when the INVITE carries no description: the
// 200 becomes the offer and the ACK the answer. The end that offered is the callee, so
// the answer in the ACK has to be produced for what the callee said - which the memory
// of it gives and reading the request body cannot.
TEST(MediaProfileTest, ADelayedOfferIsAnsweredForTheEndThatOffered) {
  ProfileFixture f("udp", "udp");

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.ok_with(kWebRtcOffer));
  f.receive(f.caller, f.ack_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);

  EXPECT_FALSE(calls.back().was_offer);
  EXPECT_EQ(calls.back().flags.target, Flags::Profile::WebRtc);
}

// Step 3 of the 2026-10-01 decision: a subscriber can say what its endpoint is, as an
// Asterisk endpoint's webrtc=yes does. It is for the leg nothing else can read -
// AthenaPhone signals over TCP and its media is WebRTC - and it decides the first offer
// towards that leg where the realm and the transport would have guessed.
TEST(MediaProfileTest, AnSubscribersProfileDecidesTheFirstOfferTowardsIt) {
  ProfileFixture f("tcp", "udp");
  f.bob->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::WebRtc);
}

// And it is the operator's word about an endpoint, not the endpoint's: once the leg has
// described itself, what it said is what it gets. A subscriber that says plain RTP for a
// phone that has since been replaced by a browser is answered as a browser.
TEST(MediaProfileTest, WhatALegSaidOutranksItsSubscriber) {
  ProfileFixture f("udp", "udp");
  f.bob->media_profile = types::MediaPolicy::Profiles::PlainRtp;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.ok_with(kWebRtcOffer));
  f.receive(f.caller, f.ack_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);
  EXPECT_FALSE(calls.back().was_offer);
  EXPECT_EQ(calls.back().flags.target, Flags::Profile::WebRtc);
}

// A subscriber's setting takes the realm's values, transport and mirror included, so an
// subscriber can opt one endpoint out of a realm's rule.
TEST(MediaProfileTest, AnSubscriberCanTakeItsLegOutOfTheTransportRule) {
  ProfileFixture f("wss", "udp");
  f.bob->media_profile = types::MediaPolicy::Profiles::Mirror;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::Mirror);
}

// Step 4 of the 2026-10-01 decision, Kamailio's failure-route pattern: a callee that
// answers an offer this node produced by guessing with 488 Not Acceptable Here (RFC 3261
// 21.4.26) is offered the other profile, once. AthenaPhone on TCP under the transport rule
// is the call this exists for: offered plain RTP, it can only refuse.
TEST(MediaProfileTest, A488ToAGuessIsReofferedWithTheOtherProfile) {
  ProfileFixture f("tcp", "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  const auto invites = ProfileFixture::requests_with(f.callee_connection, "INVITE");
  ASSERT_EQ(invites.size(), 2u) << "one re-offer to the same binding";

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::PlainRtp);
  EXPECT_TRUE(calls.back().was_offer);
  EXPECT_EQ(calls.back().flags.target, Flags::Profile::WebRtc);

  // The caller never hears about the refusal, and does hear the answer.
  EXPECT_EQ(f.response_with(f.caller_connection, 488), nullptr);
  f.receive(f.callee, f.response_to_latest(200, "OK", kWebRtcOffer));
  EXPECT_NE(f.response_with(f.caller_connection, 200), nullptr);
}

// The other profile is only worth offering if the engine can make it. A relay that does
// plain RTP and nothing else would send the refused offer again, byte for byte, which is
// what corvus-fi-1 did to AthenaPhone on 2026-10-03: the phone answered the copy with 482.
// The 488 is the answer, and the caller hears it.
TEST(MediaProfileTest, NothingIsReofferedWhenTheEngineCannotMakeTheOtherProfile) {
  ProfileFixture f("tcp", "udp");
  f.engine->plain_rtp_only_set();

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  EXPECT_EQ(ProfileFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
  EXPECT_NE(f.response_with(f.caller_connection, 488), nullptr);
  EXPECT_TRUE(f.reoffers().empty());
}

// And the same when the first offer was asked for in a profile the engine cannot make: a
// subscriber the operator marked webrtc, behind a relay that only does plain RTP. What went
// out was plain RTP whatever was asked, so plain RTP is not "the other" to try next.
TEST(MediaProfileTest, NothingIsReofferedWhenTheEngineCouldNotMakeTheFirstOfferAsAsked) {
  ProfileFixture f("tcp", "udp");
  f.engine->plain_rtp_only_set();

  f.bob->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  EXPECT_EQ(ProfileFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
  EXPECT_NE(f.response_with(f.caller_connection, 488), nullptr);
  EXPECT_TRUE(f.reoffers().empty());
}

// The first node anchors the media (TODO/ACTIVE.md, Milestone 4). A request a peer node
// forwarded has been through that node's engine already, and a second pass here would
// relay a relay - or, with one rtpengine between them, offer the same call to it twice.
TEST(MediaProfileTest, ACallAPeerNodeForwardedIsNotAnchoredAgain) {
  ProfileFixture f("tcp", "udp");
  f.caller_connection->peer = "node-b";

  f.receive(f.caller, f.invite_with_body());
  ASSERT_FALSE(ProfileFixture::requests_with(f.callee_connection, "INVITE").empty());
  EXPECT_TRUE(f.engine->calls().empty());

  // Nor is the answer on its way back.
  f.receive(f.callee, f.response_to_latest(200, "OK", kOffer));
  EXPECT_TRUE(f.engine->calls().empty());
}

// But it does let go of it. Every node a call passed through tells its engine when the call
// is over, whether or not it was the one that anchored: with one rtpengine between the
// nodes that is what lets the end of the call, reaching either node, give the ports back.
// An engine that never saw the call takes the release as nothing to do.
TEST(MediaProfileTest, ACallAPeerNodeForwardedIsStillReleasedHereWhenItEnds) {
  ProfileFixture f("tcp", "udp");
  f.caller_connection->peer = "node-b";

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(180, "Ringing"));
  f.receive(f.callee, f.response_to_latest(486, "Busy Here"));

  EXPECT_TRUE(f.engine->calls().empty());
  EXPECT_EQ(f.engine->releases(), 1);
}

// The warning that an engine cannot make a profile is for an offer the node would have had
// to convert. A WebRTC offer to a WebRTC subscriber needs nothing made: it goes through as
// it came, and saying "cannot produce webrtc - offering what it can" about it, as
// corvus-fi-1 did on every browser call to AthenaPhone, sends an operator looking for a
// fault that is not there.
TEST(MediaProfileTest, NoWarningWhenTheOfferIsAlreadyWhatTheCalleeTakes) {
  ProfileFixture f("tcp", "udp");
  f.engine->plain_rtp_only_set();

  f.bob->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite_with_body(kWebRtcOffer));
  ASSERT_FALSE(ProfileFixture::requests_with(f.callee_connection, "INVITE").empty());

  for (const auto& line : f.logger->lines(loggers::LogLevel::WARN)) {
    EXPECT_EQ(line.find("cannot produce"), std::string::npos) << line;
  }
}

// And when it does have to say so, it names the subscriber, which is what an operator can
// act on, and not the Contact of whichever device the call happened to be sent to.
TEST(MediaProfileTest, TheWarningNamesTheSubscriberTheEngineCouldNotServe) {
  ProfileFixture f("tcp", "udp");
  f.engine->plain_rtp_only_set();

  f.bob->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.receive(f.caller, f.invite_with_body());
  ASSERT_FALSE(ProfileFixture::requests_with(f.callee_connection, "INVITE").empty());

  bool warned = false;
  for (const auto& line : f.logger->lines(loggers::LogLevel::WARN)) {
    if (line.find("cannot produce") == std::string::npos) continue;
    warned = true;
    EXPECT_NE(line.find("sip:bob@example.com"), std::string::npos) << line;
  }
  EXPECT_TRUE(warned);
}

// Nothing is learned silently: the node says which subscriber needed it and what it took,
// for the operator to set as that subscriber's profile or not.
TEST(MediaProfileTest, AReofferThatWorksIsReportedForTheSubscriber) {
  ProfileFixture f("tcp", "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));
  f.receive(f.callee, f.response_to_latest(200, "OK", kWebRtcOffer));

  const auto reported = f.reoffers();
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported[0].subscriber, "sip:bob@example.com");
  EXPECT_EQ(reported[0].rejected, Flags::Profile::PlainRtp);
  ASSERT_TRUE(reported[0].took.has_value());
  EXPECT_EQ(*reported[0].took, Flags::Profile::WebRtc);
  EXPECT_EQ(reported[0].count, 1u);
}

// Under the shipped default nothing is guessed, and the callee is offered what the caller
// offered: a browser calling a desk phone hands it WebRTC. The other profile is then the
// other of what the caller said.
TEST(MediaProfileTest, AMirroredOfferRefusedIsReofferedAsTheOtherOfWhatTheCallerSaid) {
  ProfileFixture f("udp", "wss");
  f.config->behaviour.profiles = types::MediaPolicy::Profiles::Mirror;

  f.receive(f.caller, f.invite_with_body(kWebRtcOffer));
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::Mirror);
  EXPECT_EQ(calls.back().flags.target, Flags::Profile::PlainRtp);
}

// Once. A callee that refuses both has refused, and the caller is told so with the
// refusal it gave; the operator is told both were refused.
TEST(MediaProfileTest, A488ToTheReofferIsTheAnswer) {
  ProfileFixture f("tcp", "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  EXPECT_EQ(ProfileFixture::requests_with(f.callee_connection, "INVITE").size(), 2u);
  EXPECT_NE(f.response_with(f.caller_connection, 488), nullptr);

  const auto reported = f.reoffers();
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_FALSE(reported[0].took.has_value());
}

// 488 is the one refusal that is about the media. Busy is busy whatever was offered.
TEST(MediaProfileTest, OnlyA488IsReoffered) {
  ProfileFixture f("tcp", "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(486, "Busy Here"));

  EXPECT_EQ(ProfileFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
  EXPECT_NE(f.response_with(f.caller_connection, 486), nullptr);
  EXPECT_TRUE(f.reoffers().empty());
}

// A node that is not anchoring produced nothing, so there is nothing of its own to take
// back: the 488 is the callee's answer to the caller's offer, and goes to the caller.
TEST(MediaProfileTest, NothingIsReofferedWhenTheMediaIsNotAnchored) {
  ProfileFixture f("tcp", "udp");
  f.config->behaviour.anchor = false;

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(488, "Not Acceptable Here"));

  EXPECT_EQ(ProfileFixture::requests_with(f.callee_connection, "INVITE").size(), 1u);
  EXPECT_NE(f.response_with(f.caller_connection, 488), nullptr);
}

// RFC 3261 16.7: a branch that fails ends that branch, and the attempt goes on to the next
// target. The caller's call is not over until this node sends it a final response, so the
// call record - and the media anchored for it - outlives every branch but the last. A
// failed branch had been closing the call, and every later branch of a serial fork went
// out unanchored with its media released under it.
TEST(MediaProfileTest, EveryBranchOfASerialForkIsAnchored) {
  ProfileFixture f("udp", "udp");

  std::shared_ptr<MockConnection> desk_connection;
  auto desk = f.make_channel("192.0.2.21", &desk_connection, "udp");
  f.register_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.21:5060"), desk, 3600);

  f.receive(f.caller, f.invite_with_body());

  // Whichever binding was tried first rings and then refuses.
  const bool phone_first = !ProfileFixture::requests_with(f.callee_connection, "INVITE").empty();
  auto first = phone_first ? f.callee : desk;
  auto first_connection = phone_first ? f.callee_connection : desk_connection;
  auto second = phone_first ? desk : f.callee;
  auto second_connection = phone_first ? desk_connection : f.callee_connection;

  f.receive(first, f.response_to_latest(180, "Ringing", "", first_connection, "first"));
  f.receive(first, f.response_to_latest(486, "Busy Here", "", first_connection, "first"));

  ASSERT_EQ(ProfileFixture::requests_with(second_connection, "INVITE").size(), 1u);
  EXPECT_NE(f.call(), nullptr) << "the attempt is not over";
  EXPECT_EQ(f.engine->releases(), 0);

  const auto offers = f.engine->calls();
  ASSERT_EQ(offers.size(), 2u) << "the second branch's offer went through the engine";
  EXPECT_TRUE(offers[1].was_offer);

  f.receive(second, f.response_to_latest(200, "OK", kOffer, second_connection, "second"));
  ASSERT_NE(f.response_with(f.caller_connection, 200), nullptr);
  EXPECT_EQ(f.response_with(f.caller_connection, 486), nullptr);

  ASSERT_EQ(f.dialogs().size(), 1u);
  EXPECT_EQ(f.dialogs()[0]->callee_tag, "second");
  EXPECT_EQ(f.dialogs()[0]->state, types::Dialog::State::Confirmed);
}

// And when the last branch fails, the attempt is over: the caller is told, and the call
// and its media go.
TEST(MediaProfileTest, TheAttemptEndsWithTheLastBranch) {
  ProfileFixture f("udp", "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.response_to_latest(180, "Ringing"));
  f.receive(f.callee, f.response_to_latest(486, "Busy Here"));

  EXPECT_NE(f.response_with(f.caller_connection, 486), nullptr);
  EXPECT_EQ(f.call(), nullptr);
  EXPECT_TRUE(f.dialogs().empty());
  EXPECT_EQ(f.engine->releases(), 1);
}

// Step 5: what a callee said in answer to an OPTIONS is its own word, and decides the
// first offer towards it ahead of the subscriber, the realm and the transport.
// AthenaPhone answers OPTIONS with a WebRTC description whatever it signals over.
TEST(MediaProfileTest, WhatAQualifiedClientSaidDecidesTheFirstOfferTowardsIt) {
  ProfileFixture f("tcp", "udp");
  f.bob->media_profile = types::MediaPolicy::Profiles::PlainRtp;
  ASSERT_TRUE(f.store->subscriber_update(f.bob));

  f.on_strand(
      [&f]() { f.core->qualifier()->watch("sip:bob@example.com", std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), f.callee->flow_id(), 30, 3600); });
  f.timers->advance(std::chrono::milliseconds(1));
  f.settle();

  const auto probe = f.request_with(f.callee_connection, "OPTIONS");
  ASSERT_NE(probe, nullptr);

  const auto& header = probe->header;
  std::string answer = "SIP/2.0 200 OK\r\n";
  answer += "Via: " + header->headers_map["Via"][0]->to_string() + "\r\n";
  answer += "From: " + header->headers_map["From"][0]->to_string() + "\r\n";
  answer += "To: " + header->headers_map["To"][0]->to_string() + ";tag=phone\r\n";
  answer += "Call-ID: " + header->headers_map["Call-ID"][0]->to_string() + "\r\n";
  answer += "CSeq: " + header->headers_map["CSeq"][0]->to_string() + "\r\n";
  f.receive(f.callee, ProfileFixture::with_body(answer + "\r\n", kWebRtcOffer));

  f.receive(f.caller, f.invite_with_body());

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::WebRtc);
}

// RFC 3264 section 5: an INVITE with no description has the callee make the offer, in its
// 200, and that offer is produced for the caller - a leg that has said nothing yet. Its
// subscriber is then the operator's word on it, as the callee's is for an ordinary offer.
TEST(MediaProfileTest, ADelayedOfferIsProducedForTheCallersSubscriber) {
  ProfileFixture f("udp", "udp");
  f.alice->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(f.store->subscriber_update(f.alice));

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.ok_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_FALSE(calls.empty());
  EXPECT_TRUE(calls[0].was_offer);
  EXPECT_EQ(calls[0].flags.target, Flags::Profile::WebRtc);
}

// The media half of sip.localnet: a callee inside it is sent the relay's address as it
// is seen from there, and a caller outside it is left to the engine's public address.
TEST(MediaProfileTest, ALegOnTheLanIsGivenTheLocalAddressForMedia) {
  ProfileFixture f("udp", "udp");
  f.config->sip_public_address = "203.0.113.5";
  f.config->sip_localnet = {"192.0.2.20/32"};
  EXPECT_TRUE(f.config->parse_localnet());

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.ok_with(kOffer));

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);
  EXPECT_EQ(calls[0].flags.address, "192.0.2.1") << "the offer towards the callee on the LAN";
  EXPECT_EQ(calls.back().flags.address, "") << "the answer towards the caller outside";
}
