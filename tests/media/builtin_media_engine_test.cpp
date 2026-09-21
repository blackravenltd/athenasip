//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media/builtin_media_engine.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../helpers/sync_media_engine_helper.h"
#include "../mocks/logger_mock.h"
#include "config.h"
#include "media/media_engine.h"
#include "media/media_engine_drivers.h"
#include "sdp.h"

using namespace athenasip;
using athenasip::media::BuiltinMediaEngine;
using athenasip::media::Capabilities;
using athenasip::media::Flags;
using athenasip::media::MediaEngine;

namespace {

// A port range of its own per test, so two tests never contend for the same UDP ports.
// The contract is async; these tests are statements about what the engine did with an
// SDP, so they drive it through the blocking test view.
std::shared_ptr<SyncMediaEngine> make_engine(std::uint16_t port_min, std::uint16_t port_max) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=203.0.113.5&port_min=" + std::to_string(port_min) +
                                          "&port_max=" + std::to_string(port_max));

  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));
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

// The URL is the selector and stays a one-liner; anything richer comes from the
// driver's own section of the config, and what it sets wins over the URL query.
TEST(BuiltinMediaEngineTest, TheConfigSectionOverridesTheUrlQuery) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=203.0.113.5&port_min=23700&port_max=23740");

  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));

  YAML::Node own_root;
  own_root["public_address"] = "198.51.100.9";
  own_root["port_min"] = "23750";
  own_root["port_max"] = "23790";

  Config system(logger);
  ASSERT_TRUE(engine->configure(own_root, system));
  ASSERT_TRUE(engine->connect());

  auto call = make_call();
  const auto result = engine->offer(call, kOffer, Flags{});

  ASSERT_TRUE(result.ok);
  EXPECT_NE(result.sdp.find("IN IP4 198.51.100.9"), std::string::npos);
  EXPECT_EQ(result.sdp.find("203.0.113.5"), std::string::npos);

  engine->release(call);
  engine->close();
}

// A driver with no section of its own is configured with an empty node and must not
// mind: datastore: { url: memory:// } has to keep working.
TEST(BuiltinMediaEngineTest, AcceptsAnAbsentConfigSection) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=203.0.113.5&port_min=23800&port_max=23840");

  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));

  Config system(logger);
  EXPECT_TRUE(engine->configure(system.plugin_root("media", "builtin"), system));

  ASSERT_TRUE(engine->connect());

  auto call = make_call();
  const auto result = engine->offer(call, kOffer, Flags{});

  ASSERT_TRUE(result.ok);
  EXPECT_NE(result.sdp.find("IN IP4 203.0.113.5"), std::string::npos);

  engine->release(call);
  engine->close();
}

TEST(BuiltinMediaEngineTest, ConnectsAndReportsItself) {
  auto engine = make_engine(23100, 23140);

  EXPECT_TRUE(engine->is_connected());
  EXPECT_EQ(engine->name(), "builtin");
  EXPECT_FALSE(engine->version().empty());

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

// RFC 3605. The relay's RTCP port comes out of the same pool as its RTP port and is not
// reliably the one above it, so an endpoint left to assume the convention would send its
// receiver reports into somebody else's call. The attribute is written whether or not
// the far end offered one.
TEST(BuiltinMediaEngineTest, AlwaysNamesTheRtcpPortItListensOn) {
  auto engine = make_engine(23850, 23890);
  auto call = make_call();

  const char* offer_without_rtcp =
      "v=0\r\n"
      "o=alice 2890844526 2890844526 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "c=IN IP4 198.51.100.1\r\n"
      "t=0 0\r\n"
      "m=audio 49170 RTP/AVP 0\r\n"
      "a=rtpmap:0 PCMU/8000\r\n";

  const auto result = engine->offer(call, offer_without_rtcp, Flags{});
  ASSERT_TRUE(result.ok) << result.error;

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(result.sdp));
  ASSERT_EQ(rewritten.media().size(), 1u);

  std::string rtcp;
  for (const auto& attribute : rewritten.media()[0].attributes()) {
    if (attribute.rfind("rtcp:", 0) == 0) rtcp = attribute;
  }

  ASSERT_FALSE(rtcp.empty()) << "no a=rtcp in " << result.sdp;

  const auto port = std::stoul(rtcp.substr(5, rtcp.find(' ') - 5));
  EXPECT_GE(port, 23850u);
  EXPECT_LT(port, 23890u);
  EXPECT_NE(port, rewritten.media()[0].description.port);

  engine->release(call);
}

// What a bridge is for. Each leg is told where to send by the description it is given
// back, and a packet one leg sends has to come out at the other. Nothing here assumes
// how the relay is arranged - only that the two ports the two legs were handed carry
// media between them.
TEST(BuiltinMediaEngineTest, BridgesMediaBetweenTheTwoLegs) {
  auto engine = make_engine(23900, 23940);
  auto call = make_call();

  // The caller's offer goes on to the callee, so the port in what comes back is where
  // the callee sends.
  Flags from_caller;
  from_caller.participant = 0;

  const auto to_callee = engine->offer(call, kOffer, from_caller);
  ASSERT_TRUE(to_callee.ok) << to_callee.error;

  const char* answer =
      "v=0\r\n"
      "o=bob 2890844600 2890844600 IN IP4 198.51.100.2\r\n"
      "s=-\r\n"
      "c=IN IP4 198.51.100.2\r\n"
      "t=0 0\r\n"
      "m=audio 6000 RTP/AVP 0\r\n"
      "a=rtpmap:0 PCMU/8000\r\n"
      "a=rtcp:6001\r\n";

  Flags from_callee;
  from_callee.participant = 1;

  const auto to_caller = engine->answer(call, answer, from_callee);
  ASSERT_TRUE(to_caller.ok) << to_caller.error;

  SDP callee_side, caller_side;
  ASSERT_TRUE(callee_side.parse(to_callee.sdp));
  ASSERT_TRUE(caller_side.parse(to_caller.sdp));

  boost::asio::io_context io;
  boost::asio::ip::udp::socket caller(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  boost::asio::ip::udp::socket callee(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));

  const boost::asio::ip::udp::endpoint caller_sends_to(boost::asio::ip::make_address("127.0.0.1"), caller_side.media()[0].description.port);
  const boost::asio::ip::udp::endpoint callee_sends_to(boost::asio::ip::make_address("127.0.0.1"), callee_side.media()[0].description.port);

  // A relay learns where a leg is from the first packet it sends, so the first packet
  // each way is what registers the leg rather than what crosses. Real RTP behaves the
  // same: the first few packets are lost while both ends latch.
  auto pump = [&]() {
    caller.send_to(boost::asio::buffer("from-caller", 11), caller_sends_to);
    callee.send_to(boost::asio::buffer("from-callee", 11), callee_sends_to);
  };

  auto received = [&](boost::asio::ip::udp::socket& socket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
      if (socket.available() > 0) {
        char buffer[64] = {};
        boost::asio::ip::udp::endpoint from;
        const auto bytes = socket.receive_from(boost::asio::buffer(buffer), from);
        return std::string(buffer, bytes);
      }

      pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return std::string();
  };

  EXPECT_EQ(received(callee), "from-caller");
  EXPECT_EQ(received(caller), "from-callee");

  engine->release(call);
}

// What an offer needs is in the offer. Reading it from the transport would be wrong in
// both directions: a browser reaches a node over WSS and a desk phone can too, and the
// same browser offer relayed in over UDP by another proxy still wants ICE and DTLS.
TEST(MediaFlagsTest, ReadsWhatAWebRtcOfferAsksFor) {
  const char* webrtc =
      "v=0\r\n"
      "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=ice-ufrag:4ZcD\r\n"
      "a=fingerprint:sha-256 AA:BB:CC\r\n"
      "a=rtcp-mux\r\n";

  const auto flags = Flags::from_sdp(webrtc);

  EXPECT_TRUE(flags.ice);
  EXPECT_TRUE(flags.dtls);
  EXPECT_TRUE(flags.srtp);
  EXPECT_TRUE(flags.rtcp_mux);
}

TEST(MediaFlagsTest, PlainRtpAsksForNothing) {
  const auto flags = Flags::from_sdp(kOffer);

  EXPECT_FALSE(flags.ice);
  EXPECT_FALSE(flags.dtls);
  EXPECT_FALSE(flags.srtp);
  EXPECT_FALSE(flags.rtcp_mux);
}

// RFC 4568: SDES puts the keys in the description and the profile is a plain secure one.
// There is no DTLS and no ICE in it, and saying otherwise would send the offer to the
// wrong engine.
TEST(MediaFlagsTest, TheSecureProfileAloneIsEnoughToSaySrtp) {
  const char* sdes =
      "v=0\r\n"
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "c=IN IP4 198.51.100.1\r\n"
      "t=0 0\r\n"
      "m=audio 49170 RTP/SAVP 0\r\n"
      "a=rtpmap:0 PCMU/8000\r\n";

  const auto flags = Flags::from_sdp(sdes);

  EXPECT_TRUE(flags.srtp);
  EXPECT_FALSE(flags.ice);
  EXPECT_FALSE(flags.dtls);
}

// Nothing readable claims nothing. The engine that is handed the description is what
// refuses it, and guessing here would have it refuse for the wrong reason.
TEST(MediaFlagsTest, AnUnreadableDescriptionClaimsNothing) {
  const auto flags = Flags::from_sdp("this is not a session description");

  EXPECT_FALSE(flags.ice);
  EXPECT_FALSE(flags.dtls);
  EXPECT_FALSE(flags.srtp);
  EXPECT_FALSE(flags.rtcp_mux);
}

// RFC 8866 section 5.2: the o= line gives "an address of the machine from which the
// session was created". A node anchoring media so that neither end learns the other's
// address was handing one of them away in it regardless - every c= and m= was rewritten
// and the o= was not.
//
// The RFC allows the substitution outright: "For privacy reasons, it is sometimes
// desirable to obfuscate the username and IP address of the session originator. If this
// is a concern, an arbitrary <username> and private <unicast-address> MAY be chosen to
// populate the o= line, provided that these are selected in a manner that does not
// affect the global uniqueness of the field."
TEST(BuiltinMediaEngineTest, TheOriginAddressIsThisNodeAndNotTheEndpoint) {
  auto engine = make_engine(23900, 23940);
  auto call = make_call();

  const auto result = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(result.ok);

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(result.sdp));

  const auto origin = rewritten.origin();
  EXPECT_EQ(origin.address, "203.0.113.5");
  EXPECT_EQ(origin.nettype, "IN");
  EXPECT_EQ(origin.addrtype, "IP4");

  // The endpoint's address appears nowhere at all now, which is the whole point.
  EXPECT_EQ(result.sdp.find("198.51.100.1"), std::string::npos);

  engine->release(call);
  engine->close();
}

// The same paragraph's condition: the substitution must not "affect the global
// uniqueness of the field". Uniqueness is the tuple of username, session id, nettype,
// addrtype and address, so replacing the address alone leaves the endpoint's username
// and session id to carry it - two calls through this node stay distinguishable.
TEST(BuiltinMediaEngineTest, TheOriginKeepsWhatMakesItUnique) {
  auto engine = make_engine(23950, 23990);
  auto call = make_call();

  const auto result = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(result.ok);

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(result.sdp));

  const auto origin = rewritten.origin();
  EXPECT_EQ(origin.username, "alice");
  EXPECT_EQ(origin.sessionId, "2890844526");

  // The version is the endpoint's as well. RFC 3264 section 8's rule for incrementing it
  // when this node changes a re-offer is its own item, and is not this.
  EXPECT_EQ(origin.sessionVersion, "2890844526");

  engine->release(call);
  engine->close();
}

// A phone that loses power sends no BYE. Nothing in the signalling plane will ever say
// the call ended, so a proxy holding relay ports for it holds them until it restarts -
// and the ports are a finite pool. The media plane knows what the signalling plane
// cannot: whether anything is still crossing the relay.
//
// query() carries it, because it is already the contract's diagnostics call and a new
// field in its document costs no version of the plugin contract.
TEST(BuiltinMediaEngineTest, QueryReportsHowLongTheMediaHasBeenSilent) {
  auto engine = make_engine(24000, 24040);
  auto call = make_call();

  // A call the engine holds nothing for has no media to be idle, and says so rather than
  // reporting a number a caller might act on.
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":null"), std::string::npos);

  ASSERT_TRUE(engine->offer(call, kOffer, Flags{}).ok);

  // Freshly allocated, and measured from allocation rather than from zero: a call whose
  // media has not begun yet reads as young, not as infinitely idle.
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":0"), std::string::npos);

  engine->release(call);
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":null"), std::string::npos);
}

// The reading is what a packet arriving resets, and any relay of the call still carrying
// keeps the call live: the figure is the shortest idle of all of them. RTCP is relayed
// through a set of its own and counts the same, which is what stops a call on hold or one
// whose codec suppresses silence from reading as dead (RFC 3550 section 6: reports are
// sent for the life of the session whether or not there is anything to carry).
TEST(BuiltinMediaEngineTest, APacketArrivingIsWhatSaysTheCallIsAlive) {
  auto engine = make_engine(24050, 24090);
  auto call = make_call();

  const auto mapped = engine->offer(call, kOffer, Flags{});
  ASSERT_TRUE(mapped.ok) << mapped.error;

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(mapped.sdp));
  const auto rtp_port = rewritten.media()[0].description.port;
  ASSERT_NE(rtp_port, 0);

  boost::asio::io_context io;
  boost::asio::ip::udp::socket leg(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  const boost::asio::ip::udp::endpoint relay(boost::asio::ip::make_address("127.0.0.1"), rtp_port);

  leg.send_to(boost::asio::buffer("packet", 6), relay);

  // The relay reads on the global io_context thread, so the store lands a moment after
  // the send returns.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool seen = false;

  while (std::chrono::steady_clock::now() < deadline && !seen) {
    seen = engine->query(call).find("\"idle_seconds\":0") != std::string::npos;
    if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(seen) << "query said " << engine->query(call);

  engine->release(call);
}
