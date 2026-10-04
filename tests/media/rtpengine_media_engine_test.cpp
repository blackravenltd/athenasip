//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media/rtpengine_media_engine.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <boost/json.hpp>
#include <ctime>
#include <memory>
#include <set>
#include <string>

#include "../helpers/fake_rtpengine_helper.h"
#include "../helpers/sync_media_engine_helper.h"
#include "../mocks/logger_mock.h"
#include "call.h"
#include "config.h"
#include "media/bencode.h"
#include "media/media_engine.h"
#include "media/media_engine_drivers.h"

using namespace athenasip;
using athenasip::media::Bencode;
using athenasip::media::Flags;
using athenasip::media::MediaEngine;
using athenasip::media::RtpengineMediaEngine;

namespace {

const std::string kOffer =
    "v=0\r\n"
    "o=alice 2890844526 2890844526 IN IP4 198.51.100.1\r\n"
    "s=-\r\n"
    "c=IN IP4 198.51.100.1\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0\r\n";

// A two-party call as the proxy hands one to the engine: one dialog, both legs on it, the caller marked as
// originator (RFC 3261 12.1.1).
std::shared_ptr<Call> make_call() {
  auto dialog = std::make_shared<types::Dialog>();
  dialog->call_id = "call-1@example.com";
  dialog->caller_tag = "alice-tag";
  dialog->callee_tag = "bob-tag";

  auto call = std::make_shared<Call>();
  call->id = dialog->call_id;

  auto& caller = call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
  caller.dialog = dialog;

  auto& callee = call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"), nullptr, false);
  callee.dialog = dialog;

  return call;
}

std::shared_ptr<SyncMediaEngine> make_engine(const FakeRtpengine& fake, const std::string& settings = "timeout_ms: 100\nattempts: 3") {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("rtpengine://127.0.0.1:" + std::to_string(fake.port()));

  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<RtpengineMediaEngine>(logger, url));

  EXPECT_TRUE(engine->configure(YAML::Load(settings), Config(logger)));

  return engine;
}

Bencode ok_with_sdp(const std::string& sdp) { return Bencode::dictionary({{"result", Bencode(std::string("ok"))}, {"sdp", Bencode(sdp)}}); }

}  // namespace

// The ng handshake. A UDP socket opens whether or not anything listens, so only the ping shows the engine
// is reachable.
TEST(RtpengineMediaEngineTest, ConnectPingsAndWaitsForAPong) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));

  auto engine = make_engine(fake);

  ASSERT_TRUE(engine->connect());
  EXPECT_TRUE(engine->is_connected());

  auto ping = fake.last("ping");
  ASSERT_TRUE(ping.has_value());
  EXPECT_EQ(ping->string_at("command"), "ping");

  engine->close();
}

TEST(RtpengineMediaEngineTest, ConnectFailsWhenNothingAnswers) {
  FakeRtpengine fake;
  fake.swallow(5);

  auto engine = make_engine(fake, "timeout_ms: 50\nattempts: 2");

  EXPECT_FALSE(engine->connect());
  EXPECT_FALSE(engine->is_connected());

  // Asked twice, then given up on.
  EXPECT_EQ(fake.count(), 2u);
}

// Anything but pong is not a usable engine.
TEST(RtpengineMediaEngineTest, ConnectFailsWhenThePingIsAnsweredWithAnythingElse) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("error"))}, {"error-reason", Bencode(std::string("no"))}}));

  auto engine = make_engine(fake);

  EXPECT_FALSE(engine->connect());
  EXPECT_FALSE(engine->is_connected());
}

TEST(RtpengineMediaEngineTest, AnOfferNamesTheCallTheOffererAndTheDescription) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=alice 1 1 IN IP4 203.0.113.9\r\ns=-\r\nc=IN IP4 203.0.113.9\r\nt=0 0\r\nm=audio 30000 RTP/AVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  auto call = make_call();

  Flags flags;
  flags.participant = 0;  // The caller's own description.

  const auto result = engine->offer(call, kOffer, flags);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.sdp.find("203.0.113.9"), std::string::npos);

  auto request = fake.last("offer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("call-id"), "call-1@example.com");
  EXPECT_EQ(request->string_at("from-tag"), "alice-tag");
  EXPECT_EQ(request->string_at("sdp"), kOffer);

  // The two substitutions that keep one end from learning the other's address (RFC 8866 5.2).
  const auto* replace = request->find("replace");
  ASSERT_NE(replace, nullptr);
  ASSERT_TRUE(replace->is_list());

  // A list is a set here: order does not matter.
  std::set<std::string> replacements;
  for (const auto& value : replace->values()) replacements.insert(value.string());

  // RFC 3264 8: the version is rtpengine's too, because what this node emits is not what the endpoint sent.
  EXPECT_EQ(replacements, (std::set<std::string>{"origin", "sdp-version", "session-connection"}));

  engine->close();
}

// rtpengine files media under the offerer's tag, so an answer names the offerer as from and the answerer as
// to. Reversed, it silently makes a second call.
TEST(RtpengineMediaEngineTest, AnAnswerNamesTheOffererAsFromAndTheAnswererAsTo) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("answer", ok_with_sdp("v=0\r\no=bob 1 1 IN IP4 203.0.113.9\r\ns=-\r\nc=IN IP4 203.0.113.9\r\nt=0 0\r\nm=audio 30002 RTP/AVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  auto call = make_call();

  Flags flags;
  flags.participant = 1;  // The callee's answer.

  const auto result = engine->answer(call, kOffer, flags);
  ASSERT_TRUE(result.ok) << result.error;

  auto request = fake.last("answer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("from-tag"), "alice-tag");
  EXPECT_EQ(request->string_at("to-tag"), "bob-tag");

  engine->close();
}

// RFC 5761: rtcp-mux is offered onward only when the sending end asked for it.
TEST(RtpengineMediaEngineTest, RtcpMuxIsCarriedOnlyWhenTheOfferHadIt) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30000 RTP/AVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  auto call = make_call();

  Flags plain;
  plain.participant = 0;
  ASSERT_TRUE(engine->offer(call, kOffer, plain).ok);

  auto without = fake.last("offer");
  ASSERT_TRUE(without.has_value());
  EXPECT_EQ(without->find("rtcp-mux"), nullptr);

  Flags muxed;
  muxed.participant = 0;
  muxed.rtcp_mux = true;
  ASSERT_TRUE(engine->offer(call, kOffer, muxed).ok);

  auto with = fake.last("offer");
  ASSERT_TRUE(with.has_value());

  const auto* mux = with->find("rtcp-mux");
  ASSERT_NE(mux, nullptr);
  ASSERT_TRUE(mux->is_list());
  ASSERT_EQ(mux->values().size(), 1u);
  EXPECT_EQ(mux->values()[0].string(), "offer");

  engine->close();
}

// An engine's refusal comes back with its reason, so the proxy can pass the description through unanchored.
TEST(RtpengineMediaEngineTest, AnErrorReplyIsReportedWithItsReason) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", Bencode::dictionary({{"result", Bencode(std::string("error"))}, {"error-reason", Bencode(std::string("Unknown call-id"))}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  const auto result = engine->offer(make_call(), kOffer, Flags{});

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "Unknown call-id");

  engine->close();
}

TEST(RtpengineMediaEngineTest, AnOkWithNoSdpIsNotAnAnswer) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", Bencode::dictionary({{"result", Bencode(std::string("ok"))}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  EXPECT_FALSE(engine->offer(make_call(), kOffer, Flags{}).ok);

  engine->close();
}

// The delete names the call and no tag, so both legs' ports are released.
TEST(RtpengineMediaEngineTest, ReleaseDeletesTheWholeCall) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  EXPECT_TRUE(engine->release(make_call()));

  auto request = fake.last("delete");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("call-id"), "call-1@example.com");
  EXPECT_EQ(request->find("from-tag"), nullptr);
  EXPECT_EQ(request->find("to-tag"), nullptr);

  engine->close();
}

// UDP loses datagrams, and rtpengine caches its answer against the cookie, so a retry reuses the cookie.
TEST(RtpengineMediaEngineTest, ARequestLostOnTheWayIsAskedAgainWithTheSameCookie) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));

  auto engine = make_engine(fake, "timeout_ms: 50\nattempts: 3");
  ASSERT_TRUE(engine->connect());

  const auto before = fake.count();

  fake.swallow(1);
  EXPECT_TRUE(engine->release(make_call()));

  const auto cookies = fake.cookies();
  ASSERT_GE(cookies.size(), before + 2);

  // The lost request and the one that got through are the same request.
  EXPECT_EQ(cookies[before], cookies[before + 1]);

  engine->close();
}

// What the call sweep reads: the shortest "last packet" idle across the call's streams.
TEST(RtpengineMediaEngineTest, QueryReportsTheShortestIdleAcrossEveryStream) {
  const auto now = static_cast<std::int64_t>(std::time(nullptr));

  auto quiet = Bencode::dictionary({{"last packet", Bencode(now - 600)}});
  auto busy = Bencode::dictionary({{"last packet", Bencode(now - 4)}});

  auto media = Bencode::dictionary({{"streams", Bencode::list({quiet, busy})}});
  auto leg = Bencode::dictionary({{"medias", Bencode::list({media})}});

  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("query", Bencode::dictionary(
                           {{"result", Bencode(std::string("ok"))}, {"created", Bencode(now - 900)}, {"tags", Bencode::dictionary({{"alice-tag", leg}})}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  const auto document = engine->query(make_call());

  EXPECT_NE(document.find("\"engine\":\"rtpengine\""), std::string::npos) << document;
  EXPECT_NE(document.find("\"streams\":2"), std::string::npos) << document;

  // Four seconds ago, give or take the time the test took.
  EXPECT_TRUE(document.find("\"idle_seconds\":4") != std::string::npos || document.find("\"idle_seconds\":5") != std::string::npos) << document;

  engine->close();
}

// The ng query gives each stream a "stats" dictionary of what the engine received from that end. What it
// sent is reported only where the engine reports it, as "stats_out"; nothing is derived from the other leg.
TEST(RtpengineMediaEngineTest, QueryReportsEachEndsCounts) {
  auto with_out = Bencode::dictionary({{"stats", Bencode::dictionary({{"packets", Bencode(1427)}, {"bytes", Bencode(84790)}, {"errors", Bencode(0)}})},
                                       {"stats_out", Bencode::dictionary({{"packets", Bencode(4)}, {"bytes", Bencode(336)}})}});
  auto without_out = Bencode::dictionary({{"stats", Bencode::dictionary({{"packets", Bencode(4)}, {"bytes", Bencode(336)}, {"errors", Bencode(0)}})}});

  auto phone = Bencode::dictionary({{"medias", Bencode::list({Bencode::dictionary({{"streams", Bencode::list({with_out})}})})}});
  auto browser = Bencode::dictionary({{"medias", Bencode::list({Bencode::dictionary({{"streams", Bencode::list({without_out})}})})}});

  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("query", Bencode::dictionary({{"result", Bencode(std::string("ok"))}, {"tags", Bencode::dictionary({{"a", phone}, {"b", browser}})}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  const auto document = boost::json::parse(engine->query(make_call())).as_object();
  ASSERT_TRUE(document.contains("legs")) << boost::json::serialize(document);

  const auto& legs = document.at("legs").as_array();
  ASSERT_EQ(legs.size(), 2u);

  const auto& first = legs[0].as_object();
  EXPECT_EQ(first.at("packets_in").as_int64(), 1427);
  EXPECT_EQ(first.at("bytes_in").as_int64(), 84790);
  EXPECT_EQ(first.at("packets_out").as_int64(), 4);
  EXPECT_EQ(first.at("bytes_out").as_int64(), 336);

  const auto& second = legs[1].as_object();
  EXPECT_EQ(second.at("packets_in").as_int64(), 4);
  EXPECT_FALSE(second.contains("packets_out")) << "an engine that does not say what it sent is not made to";

  engine->close();
}

// A stream with no packet yet is idle since the call was created, as the builtin relay reports, so a call
// that never carried media does not live for ever.
TEST(RtpengineMediaEngineTest, AStreamWithNoPacketIsIdleSinceTheCallWasCreated) {
  const auto now = static_cast<std::int64_t>(std::time(nullptr));

  auto silent = Bencode::dictionary({{"last packet", Bencode(static_cast<std::int64_t>(0))}});
  auto media = Bencode::dictionary({{"streams", Bencode::list({silent})}});
  auto leg = Bencode::dictionary({{"medias", Bencode::list({media})}});

  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("query", Bencode::dictionary(
                           {{"result", Bencode(std::string("ok"))}, {"created", Bencode(now - 300)}, {"tags", Bencode::dictionary({{"alice-tag", leg}})}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  const auto document = engine->query(make_call());
  EXPECT_NE(document.find("\"idle_seconds\":30"), std::string::npos) << document;

  engine->close();
}

// A call with no streams reports no idle figure, which the sweep reads as "no answer".
TEST(RtpengineMediaEngineTest, ACallWithNoStreamsReportsNoIdleAtAll) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("query", Bencode::dictionary({{"result", Bencode(std::string("ok"))}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  const auto document = engine->query(make_call());
  EXPECT_NE(document.find("\"idle_seconds\":null"), std::string::npos) << document;

  engine->close();
}

TEST(RtpengineMediaEngineTest, RecordingIsAskedForByName) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  auto call = make_call();

  EXPECT_TRUE(engine->start_recording(call).ok);
  EXPECT_TRUE(engine->stop_recording(call).ok);

  auto started = fake.last("start recording");
  ASSERT_TRUE(started.has_value());
  EXPECT_EQ(started->string_at("call-id"), "call-1@example.com");

  EXPECT_TRUE(fake.last("stop recording").has_value());

  engine->close();
}

// The profile tells the engine which side of the bridge is the browser. Without it rtpengine mirrors what it
// was handed, and a desk phone gets a WebRTC offer.
TEST(RtpengineMediaEngineTest, TheWebRtcProfileAsksRtpengineForIceDtlsAndSavpf) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30000 UDP/TLS/RTP/SAVPF 111\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  Flags flags;
  flags.participant = 0;
  flags.target = Flags::Profile::WebRtc;

  ASSERT_TRUE(engine->offer(make_call(), kOffer, flags).ok);

  auto request = fake.last("offer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("ICE"), "force");

  // Passive in the offer, so the engine advertises actpass. Only in the offer: see the answer test below.
  EXPECT_EQ(request->string_at("DTLS"), "passive");
  EXPECT_EQ(request->string_at("transport-protocol"), "UDP/TLS/RTP/SAVPF");

  const auto* mux = request->find("rtcp-mux");
  ASSERT_NE(mux, nullptr);
  ASSERT_EQ(mux->values().size(), 2u);
  EXPECT_EQ(mux->values()[0].string(), "offer");
  EXPECT_EQ(mux->values()[1].string(), "require");

  engine->close();
}

// RFC 5763 5: the offerer says actpass and the answerer chooses. Towards an offerer rtpengine is the answerer
// and has already chosen active and started the handshake when ICE came up. Sending "passive" in the answer
// would flip the role under a handshake in flight, and neither end would start again.
TEST(RtpengineMediaEngineTest, TheWebRtcAnswerLeavesTheDtlsRoleToTheEngine) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("answer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30002 UDP/TLS/RTP/SAVPF 111\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  Flags flags;
  flags.participant = 1;
  flags.target = Flags::Profile::WebRtc;

  ASSERT_TRUE(engine->answer(make_call(), kOffer, flags).ok);

  auto request = fake.last("answer");
  ASSERT_TRUE(request.has_value());

  // Everything else the profile says still applies to the answer.
  EXPECT_EQ(request->string_at("ICE"), "force");
  EXPECT_EQ(request->string_at("transport-protocol"), "UDP/TLS/RTP/SAVPF");

  // The role does not: the engine already chose it.
  EXPECT_EQ(request->find("DTLS"), nullptr);

  engine->close();
}

TEST(RtpengineMediaEngineTest, ThePlainRtpProfileStripsWhatAPhoneCannotUse) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30000 RTP/AVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  Flags flags;
  flags.participant = 0;
  flags.target = Flags::Profile::PlainRtp;

  // Even when the offer in hand is a browser's.
  flags.ice = true;
  flags.dtls = true;
  flags.rtcp_mux = true;

  ASSERT_TRUE(engine->offer(make_call(), kOffer, flags).ok);

  auto request = fake.last("offer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("ICE"), "remove");
  EXPECT_EQ(request->string_at("DTLS"), "off");
  EXPECT_EQ(request->string_at("transport-protocol"), "RTP/AVP");

  const auto* mux = request->find("rtcp-mux");
  ASSERT_NE(mux, nullptr);
  ASSERT_EQ(mux->values().size(), 1u);
  EXPECT_EQ(mux->values()[0].string(), "demux");

  engine->close();
}

// RFC 4568: the keys travel in the description, so there is no DTLS handshake and no ICE.
TEST(RtpengineMediaEngineTest, TheSrtpProfileAsksForSavpWithNoDtls) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30000 RTP/SAVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  Flags flags;
  flags.participant = 0;
  flags.target = Flags::Profile::SrtpSdes;

  ASSERT_TRUE(engine->offer(make_call(), kOffer, flags).ok);

  auto request = fake.last("offer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->string_at("transport-protocol"), "RTP/SAVP");
  EXPECT_EQ(request->string_at("DTLS"), "off");
  EXPECT_EQ(request->string_at("ICE"), "remove");

  engine->close();
}

// The mirror profile sends no ICE or DTLS instruction, leaving rtpengine to mirror what it was handed.
TEST(RtpengineMediaEngineTest, TheMirrorProfileSaysNothingAboutIceOrDtls) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp("v=0\r\no=- 1 1 IN IP4 203.0.113.9\r\ns=-\r\nt=0 0\r\nm=audio 30000 RTP/AVP 0\r\n"));

  auto engine = make_engine(fake);
  ASSERT_TRUE(engine->connect());

  Flags flags;
  flags.participant = 0;
  flags.target = Flags::Profile::Mirror;

  ASSERT_TRUE(engine->offer(make_call(), kOffer, flags).ok);

  auto request = fake.last("offer");
  ASSERT_TRUE(request.has_value());

  EXPECT_EQ(request->find("ICE"), nullptr);
  EXPECT_EQ(request->find("DTLS"), nullptr);
  EXPECT_EQ(request->find("transport-protocol"), nullptr);

  engine->close();
}

TEST(RtpengineMediaEngineTest, CapabilitiesAreBridgeRecordAndTranscodeButNotConference) {
  FakeRtpengine fake;
  auto engine = make_engine(fake);

  const auto capabilities = engine->capabilities();

  EXPECT_TRUE(capabilities.bridge);
  EXPECT_TRUE(capabilities.record);
  EXPECT_TRUE(capabilities.transcode);
  EXPECT_FALSE(capabilities.conference);
}

// An engine that cannot record declines rather than silently succeed.
TEST(RtpengineMediaEngineTest, AnEngineThatDoesNotRecordDeclines) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&port_min=24800&port_max=24810");

  SyncMediaEngine builtin(std::make_shared<media::BuiltinMediaEngine>(logger, url));

  EXPECT_FALSE(builtin.start_recording(make_call()).ok);
  EXPECT_FALSE(builtin.stop_recording(make_call()).ok);
}

TEST(RtpengineMediaEngineTest, TheDriverResolvesFromItsUrlScheme) {
  auto logger = std::make_shared<MockLogger>();
  media::register_builtin_media_engines(logger);

  auto engine = MediaEngine::create_driver(logger, "rtpengine://127.0.0.1:2223");

  ASSERT_NE(engine, nullptr);
  EXPECT_EQ(engine->name(), "rtpengine");
}

// A URL with no host is a configuration error at startup.
TEST(RtpengineMediaEngineTest, AUrlWithNoHostIsRefusedAtConfigureTime) {
  auto logger = std::make_shared<MockLogger>();
  auto engine = std::make_shared<RtpengineMediaEngine>(logger, std::make_shared<types::URL>("rtpengine://"));

  EXPECT_FALSE(engine->configure(YAML::Node(), Config(logger)));
}

// A leg inside sip.localnet (Flags::address) is told the local address; every other leg the configured
// media_address. One address for both would depend on the router hairpinning, or be unreachable from outside.
TEST(RtpengineMediaEngineTest, ALegInsideTheLocalNetworkIsGivenTheLocalAddress) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp(kOffer));

  auto engine = make_engine(fake, "timeout_ms: 100\nattempts: 3\nmedia_address: 203.0.113.5");
  ASSERT_TRUE(engine->connect());
  auto call = make_call();

  Flags outside;
  outside.participant = 0;
  ASSERT_TRUE(engine->offer(call, kOffer, outside).ok);
  EXPECT_EQ(fake.last("offer")->string_at("media-address"), "203.0.113.5");

  Flags inside;
  inside.participant = 0;
  inside.address = "10.44.1.50";
  ASSERT_TRUE(engine->offer(call, kOffer, inside).ok);
  EXPECT_EQ(fake.last("offer")->string_at("media-address"), "10.44.1.50");

  engine->close();
}

// media_address may be a name: rtpengine is given the address it resolves to, because phones do not resolve
// a name in a description.
TEST(RtpengineMediaEngineTest, AMediaAddressThatIsANameIsGivenAsTheAddressItResolvesTo) {
  FakeRtpengine fake;
  fake.answer("ping", Bencode::dictionary({{"result", Bencode(std::string("pong"))}}));
  fake.answer("offer", ok_with_sdp(kOffer));

  auto engine = make_engine(fake, "timeout_ms: 100\nattempts: 3\nmedia_address: localhost");
  ASSERT_TRUE(engine->connect());
  auto call = make_call();

  Flags flags;
  flags.participant = 0;
  ASSERT_TRUE(engine->offer(call, kOffer, flags).ok);
  EXPECT_EQ(fake.last("offer")->string_at("media-address"), "127.0.0.1");

  engine->close();
}
