//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>

#include "media/builtin_media_engine.h"
#include "media/media_engine.h"
#include "media/media_engine_drivers.h"
#include "sdp.h"

#include "../mocks/logger_mock.h"

using namespace athenasip;
using athenasip::media::BuiltinMediaEngine;
using athenasip::media::Capabilities;
using athenasip::media::Flags;
using athenasip::media::MediaEngine;

namespace {

// A port range of its own per test, so two tests never contend for the same UDP ports.
std::shared_ptr<BuiltinMediaEngine> make_engine(std::uint16_t port_min, std::uint16_t port_max) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=203.0.113.5&port_min=" + std::to_string(port_min) +
                                          "&port_max=" + std::to_string(port_max));

  auto engine = std::make_shared<BuiltinMediaEngine>(logger, url);
  engine->connect();
  return engine;
}

std::shared_ptr<Call> make_call() {
  auto call = std::make_shared<Call>();
  call->id = "call-media-1";
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  return call;
}

const char* kOffer =
    "v=0\r\n"
    "o=alice 2890844526 2890844526 IN IP4 198.51.100.1\r\n"
    "s=-\r\n"
    "c=IN IP4 198.51.100.1\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0 8 101\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtcp:49171\r\n";

}  // namespace

TEST(BuiltinMediaEngineTest, ConnectsAndReportsItself) {
  auto engine = make_engine(23100, 23140);

  EXPECT_TRUE(engine->is_connected());
  EXPECT_FALSE(engine->get_driver_name().empty());

  engine->close();
  EXPECT_FALSE(engine->is_connected());
}

// The builtin engine bridges and nothing else. A caller needing conference, recording
// or transcoding has to pick a different driver, and can tell in advance.
TEST(BuiltinMediaEngineTest, AdvertisesBridgeOnly) {
  auto engine = make_engine(23150, 23190);
  const auto capabilities = engine->capabilities();

  EXPECT_TRUE(capabilities.bridge);
  EXPECT_FALSE(capabilities.conference);
  EXPECT_FALSE(capabilities.record);
  EXPECT_FALSE(capabilities.transcode);
}

TEST(BuiltinMediaEngineTest, CapabilityCheckIsUsableByCallers) {
  Capabilities engine;
  engine.bridge = true;

  Capabilities needs_bridge;
  needs_bridge.bridge = true;
  EXPECT_TRUE(engine.supports_all_of(needs_bridge));

  Capabilities needs_conference;
  needs_conference.conference = true;
  EXPECT_FALSE(engine.supports_all_of(needs_conference));
}

// The engine puts itself in the media path: the connection address and the media port
// both become ours, so RTP arrives here rather than going end to end.
TEST(BuiltinMediaEngineTest, OfferRewritesTheMediaPathToThisNode) {
  auto engine = make_engine(23200, 23240);
  auto call = make_call();

  Flags flags;
  flags.participant = 0;

  auto result = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(result.ok) << result.error;

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(result.sdp));

  EXPECT_EQ(rewritten.connection().address, "203.0.113.5");
  ASSERT_EQ(rewritten.media().size(), 1u);
  EXPECT_NE(rewritten.media()[0].description.port, 49170);
  EXPECT_GE(rewritten.media()[0].description.port, 23200);
  EXPECT_LT(rewritten.media()[0].description.port, 23240);
}

TEST(BuiltinMediaEngineTest, OfferRewritesTheRtcpAttribute) {
  auto engine = make_engine(23250, 23290);
  auto call = make_call();

  auto result = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_NE(result.sdp.find("a=rtcp:"), std::string::npos);
  EXPECT_EQ(result.sdp.find("a=rtcp:49171"), std::string::npos);
  EXPECT_NE(result.sdp.find("IN IP4 203.0.113.5"), std::string::npos);
}

TEST(BuiltinMediaEngineTest, StreamsAreHeldAgainstTheNamedParticipant) {
  auto engine = make_engine(23300, 23340);
  auto call = make_call();

  Flags flags;
  flags.participant = 1;

  ASSERT_TRUE(engine->offer(call, kOffer, flags).ok);

  EXPECT_TRUE(call->participants[0].streams.empty());
  EXPECT_EQ(call->participants[1].streams.size(), 1u);
}

TEST(BuiltinMediaEngineTest, RepeatedOfferReusesTheSameRelay) {
  auto engine = make_engine(23350, 23390);
  auto call = make_call();

  auto first = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(first.ok);

  auto second = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(second.ok);

  // One stream, not two, and the same port both times.
  EXPECT_EQ(call->participants[0].streams.size(), 1u);

  SDP a, b;
  ASSERT_TRUE(a.parse(first.sdp));
  ASSERT_TRUE(b.parse(second.sdp));
  EXPECT_EQ(a.media()[0].description.port, b.media()[0].description.port);
}

// Plain RTP only: anything needing ICE, DTLS or SRTP is rtpengine's job, and the
// engine has to say so rather than quietly producing a broken answer.
TEST(BuiltinMediaEngineTest, DeclinesWebRtcFlags) {
  auto engine = make_engine(23400, 23440);
  auto call = make_call();

  Flags ice;
  ice.ice = true;
  EXPECT_FALSE(engine->offer(call, kOffer, ice).ok);

  Flags dtls;
  dtls.dtls = true;
  EXPECT_FALSE(engine->answer(call, kOffer, dtls).ok);

  Flags srtp;
  srtp.srtp = true;
  EXPECT_FALSE(engine->offer(call, kOffer, srtp).ok);
}

TEST(BuiltinMediaEngineTest, RejectsBadInput) {
  auto engine = make_engine(23450, 23490);
  auto call = make_call();

  EXPECT_FALSE(engine->offer(nullptr, kOffer, Flags{}).ok);

  Flags out_of_range;
  out_of_range.participant = 99;
  EXPECT_FALSE(engine->offer(call, kOffer, out_of_range).ok);
}

TEST(BuiltinMediaEngineTest, ReleaseGivesTheRelaysBack) {
  auto engine = make_engine(23500, 23540);
  auto call = make_call();

  ASSERT_TRUE(engine->offer(call, kOffer, Flags{}).ok);
  EXPECT_EQ(call->participants[0].streams.size(), 1u);

  EXPECT_TRUE(engine->release(call));
  EXPECT_TRUE(call->participants[0].streams.empty());

  // Releasing something the engine never saw is not an error.
  auto unknown = std::make_shared<Call>();
  unknown->id = "never-seen";
  EXPECT_TRUE(engine->release(unknown));
}

TEST(BuiltinMediaEngineTest, QueryReportsWhatIsHeld) {
  auto engine = make_engine(23550, 23590);
  auto call = make_call();

  ASSERT_TRUE(engine->offer(call, kOffer, Flags{}).ok);

  const auto before = engine->query(call);
  EXPECT_NE(before.find(call->id), std::string::npos);
  EXPECT_NE(before.find("\"relay_sets\":2"), std::string::npos);

  engine->release(call);
  EXPECT_NE(engine->query(call).find("\"relay_sets\":0"), std::string::npos);
}

// A bridge-only driver must decline the conference operations rather than pretend.
TEST(BuiltinMediaEngineTest, ConferenceOperationsAreDeclined) {
  auto engine = make_engine(23600, 23640);
  auto call = make_call();

  EXPECT_FALSE(engine->join(call, 0));
  EXPECT_FALSE(engine->leave(call, 0));
  EXPECT_TRUE(engine->roster(call).empty());
}

TEST(BuiltinMediaEngineTest, ResolvesThroughTheDriverRegistry) {
  auto logger = std::make_shared<MockLogger>();
  media::register_builtin_media_engines(logger);

  auto engine = MediaEngine::create_driver(logger, "builtin://?port_min=23650&port_max=23690");
  ASSERT_NE(engine, nullptr);
  EXPECT_TRUE(engine->capabilities().bridge);

  EXPECT_EQ(MediaEngine::create_driver(logger, "nosuchscheme://host"), nullptr);
}
