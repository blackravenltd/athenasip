//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../helpers/proxy_fixture_helper.h"
#include "media/media_engine.h"

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
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void query(plugins::Executor on, std::shared_ptr<athenasip::Call> call, plugins::Handler<std::string> handler) override {
    (void)call;
    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success("{}"));
  }

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
  bool _connected = false;
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
