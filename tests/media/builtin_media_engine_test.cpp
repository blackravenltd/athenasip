//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media/builtin_media_engine.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/json.hpp>
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

// A port range per test, so two tests never contend for the same UDP ports. Driven through the blocking test view.
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

const char* kAnswer =
    "v=0\r\n"
    "o=bob 2890844600 2890844600 IN IP4 198.51.100.2\r\n"
    "s=-\r\n"
    "c=IN IP4 198.51.100.2\r\n"
    "t=0 0\r\n"
    "m=audio 6000 RTP/AVP 0\r\n"
    "a=rtpmap:0 PCMU/8000\r\n";

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

// The URL selects the driver; the driver's own config section overrides the URL query.
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

// A driver with no config section is configured with an empty node and must accept it.
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

// The builtin engine advertises bridge only: no conference, recording or transcoding.
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

// The connection address and the media port both become this node's, so RTP arrives here.
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

// A leg on the node's own LAN is given the address that reaches the relay from there: the public address
// works for it only if the router hairpins.
TEST(BuiltinMediaEngineTest, AnAddressForTheLegReplacesThePublicOne) {
  auto engine = make_engine(23295, 23299);
  auto call = make_call();

  Flags flags;
  flags.address = "192.168.1.2";

  auto result = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(result.ok) << result.error;

  SDP rewritten;
  ASSERT_TRUE(rewritten.parse(result.sdp));
  EXPECT_EQ(rewritten.connection().address, "192.168.1.2");
  EXPECT_EQ(result.sdp.find("203.0.113.5"), std::string::npos) << result.sdp;
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

// Plain RTP only: the engine declines ICE, DTLS and SRTP rather than produce a broken answer.
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

// A bridge-only driver declines the conference operations.
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

// RFC 3605: the relay's RTCP port is not reliably RTP+1, so a=rtcp is always written, whether or not the far
// end offered one.
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

// Each leg sends to the port in the description it was given, and a packet from one leg comes out at the other.
TEST(BuiltinMediaEngineTest, BridgesMediaBetweenTheTwoLegs) {
  auto engine = make_engine(23900, 23940);
  auto call = make_call();

  // The caller's offer goes on to the callee, so the port in the result is where the callee sends.
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

  // A relay learns where a leg is from its first packet, so the first packet each way registers the leg and may
  // not cross.
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

namespace {

std::string description_at(std::uint16_t port, const std::string& who) {
  return "v=0\r\no=" + who + " 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio " + std::to_string(port) +
         " RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
}

// Waits a little for a datagram; empty when none came.
std::string datagram(boost::asio::ip::udp::socket& socket, std::chrono::milliseconds bound = std::chrono::milliseconds(500)) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (socket.available() > 0) {
      char buffer[64] = {};
      boost::asio::ip::udp::endpoint from;
      const auto bytes = socket.receive_from(boost::asio::buffer(buffer), from);
      return std::string(buffer, bytes);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::string();
}

void drain(boost::asio::ip::udp::socket& socket) {
  while (!datagram(socket, std::chrono::milliseconds(100)).empty()) {
  }
}

}  // namespace

// A leg that only listens (an IVR, a recorder, a muted phone) never sends first. Each end starts at the
// address its own description gave (RFC 8866 5.7, 5.14), so media reaches it before it has sent anything.
TEST(BuiltinMediaEngineTest, MediaReachesALegThatHasNotSentAnything) {
  auto engine = make_engine(24000, 24040);
  auto call = make_call();

  boost::asio::io_context io;
  boost::asio::ip::udp::socket caller(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  boost::asio::ip::udp::socket callee(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));

  Flags from_caller;
  from_caller.participant = 0;
  const auto to_callee = engine->offer(call, description_at(caller.local_endpoint().port(), "caller"), from_caller);
  ASSERT_TRUE(to_callee.ok) << to_callee.error;

  Flags from_callee;
  from_callee.participant = 1;
  const auto to_caller = engine->answer(call, description_at(callee.local_endpoint().port(), "callee"), from_callee);
  ASSERT_TRUE(to_caller.ok) << to_caller.error;

  SDP caller_side;
  ASSERT_TRUE(caller_side.parse(to_caller.sdp));
  const boost::asio::ip::udp::endpoint relay(boost::asio::ip::make_address("127.0.0.1"), caller_side.media()[0].description.port);

  // Only the caller ever sends.
  for (int i = 0; i < 3; ++i) caller.send_to(boost::asio::buffer("one-way", 7), relay);

  EXPECT_EQ(datagram(callee), "one-way") << "nothing reached the leg that had not sent";

  // The counters show it: out to the callee, nothing in from it.
  const auto document = boost::json::parse(engine->query(call)).as_object();
  std::int64_t silent_ends = 0;
  for (const auto& leg : document.at("legs").as_array()) {
    const auto& end = leg.as_object();
    if (end.at("packets_in").as_int64() == 0 && end.at("packets_out").as_int64() > 0) ++silent_ends;
  }
  EXPECT_EQ(silent_ends, 1) << boost::json::serialize(document);

  engine->release(call);
}

// Symmetric latching: once an end is heard from, media goes to where its packets come from, not to the
// address in its description.
TEST(BuiltinMediaEngineTest, AnEndIsFollowedToWhereItsPacketsComeFrom) {
  auto engine = make_engine(24050, 24090);
  auto call = make_call();

  boost::asio::io_context io;
  boost::asio::ip::udp::socket caller(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  boost::asio::ip::udp::socket described(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  boost::asio::ip::udp::socket actual(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));

  Flags from_caller;
  from_caller.participant = 0;
  const auto to_callee = engine->offer(call, description_at(caller.local_endpoint().port(), "caller"), from_caller);
  ASSERT_TRUE(to_callee.ok);

  Flags from_callee;
  from_callee.participant = 1;
  const auto to_caller = engine->answer(call, description_at(described.local_endpoint().port(), "callee"), from_callee);
  ASSERT_TRUE(to_caller.ok);

  SDP callee_side, caller_side;
  ASSERT_TRUE(callee_side.parse(to_callee.sdp));
  ASSERT_TRUE(caller_side.parse(to_caller.sdp));
  const boost::asio::ip::udp::endpoint caller_relay(boost::asio::ip::make_address("127.0.0.1"), caller_side.media()[0].description.port);
  const boost::asio::ip::udp::endpoint callee_relay(boost::asio::ip::make_address("127.0.0.1"), callee_side.media()[0].description.port);

  caller.send_to(boost::asio::buffer("before", 6), caller_relay);
  EXPECT_EQ(datagram(described), "before");

  // The callee speaks, from an address its description did not name.
  actual.send_to(boost::asio::buffer("hello", 5), callee_relay);
  EXPECT_EQ(datagram(caller), "hello");
  drain(described);

  caller.send_to(boost::asio::buffer("after", 5), caller_relay);
  EXPECT_EQ(datagram(actual), "after") << "media did not follow the end to where it is";
  EXPECT_TRUE(datagram(described, std::chrono::milliseconds(200)).empty()) << "media still went to the address it was described at";

  engine->release(call);
}

// The relay counts what it carried, per sending end and in total, so a relayed call can be told from one whose
// media went end to end.
TEST(BuiltinMediaEngineTest, CountsWhatItCarries) {
  auto engine = make_engine(23950, 23990);
  auto call = make_call();

  const auto relayed_before = engine->driver()->packets_relayed();
  ASSERT_TRUE(relayed_before.has_value()) << "the builtin relay is in a position to know";

  boost::asio::io_context io;
  boost::asio::ip::udp::socket caller(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  boost::asio::ip::udp::socket callee(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));

  // Each end describes the socket it really sends from, as an endpoint not behind NAT does.
  Flags from_caller;
  from_caller.participant = 0;
  const auto to_callee = engine->offer(call, description_at(caller.local_endpoint().port(), "caller"), from_caller);
  ASSERT_TRUE(to_callee.ok);

  Flags from_callee;
  from_callee.participant = 1;
  const auto to_caller = engine->answer(call, description_at(callee.local_endpoint().port(), "callee"), from_callee);
  ASSERT_TRUE(to_caller.ok);

  SDP callee_side, caller_side;
  ASSERT_TRUE(callee_side.parse(to_callee.sdp));
  ASSERT_TRUE(caller_side.parse(to_caller.sdp));
  const boost::asio::ip::udp::endpoint caller_sends_to(boost::asio::ip::make_address("127.0.0.1"), caller_side.media()[0].description.port);
  const boost::asio::ip::udp::endpoint callee_sends_to(boost::asio::ip::make_address("127.0.0.1"), callee_side.media()[0].description.port);

  // Eleven 20-byte packets each way.
  for (int i = 0; i < 11; ++i) {
    caller.send_to(boost::asio::buffer(std::string(20, 'c')), caller_sends_to);
    callee.send_to(boost::asio::buffer(std::string(20, 'e')), callee_sends_to);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto document = boost::json::parse(engine->query(call)).as_object();
  ASSERT_TRUE(document.contains("legs")) << boost::json::serialize(document);

  const auto& legs = document.at("legs").as_array();
  ASSERT_EQ(legs.size(), 2u) << boost::json::serialize(document);

  // Split by direction: in is what the end sent, out what the relay sent it. Both ends were known from their
  // descriptions, so no packet was lost to latching.
  std::int64_t out_total = 0;
  for (const auto& leg : legs) {
    const auto& end = leg.as_object();
    EXPECT_EQ(end.at("packets_in").as_int64(), 11) << boost::json::serialize(document);
    EXPECT_EQ(end.at("bytes_in").as_int64(), 220) << boost::json::serialize(document);
    EXPECT_EQ(end.at("bytes_out").as_int64(), end.at("packets_out").as_int64() * 20) << boost::json::serialize(document);
    out_total += end.at("packets_out").as_int64();
  }
  EXPECT_EQ(out_total, 22);

  const auto relayed = *engine->driver()->packets_relayed() - *relayed_before;
  EXPECT_EQ(relayed, 22u);

  engine->release(call);
}

// What an offer needs is read from the offer, not the transport it arrived on.
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

// RFC 4568: SDES puts the keys in the description under a plain secure profile, with no DTLS and no ICE.
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

// An unreadable description claims nothing; the engine handed it is what refuses it.
TEST(MediaFlagsTest, AnUnreadableDescriptionClaimsNothing) {
  const auto flags = Flags::from_sdp("this is not a session description");

  EXPECT_FALSE(flags.ice);
  EXPECT_FALSE(flags.dtls);
  EXPECT_FALSE(flags.srtp);
  EXPECT_FALSE(flags.rtcp_mux);
}

// RFC 8866 5.2: the o= line carries the originator's address, and permits substituting it for privacy. An
// anchoring node rewrites it with its own, so neither end learns the other's address.
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

  // The endpoint's address appears nowhere.
  EXPECT_EQ(result.sdp.find("198.51.100.1"), std::string::npos);

  engine->release(call);
  engine->close();
}

// RFC 8866 5.2: the substitution must not affect the field's global uniqueness, so the endpoint's username
// and session id are kept.
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

  // The version is the endpoint's too.
  EXPECT_EQ(origin.sessionVersion, "2890844526");

  engine->release(call);
  engine->close();
}

// RFC 3264 8: the o= version MUST be incremented when the description changes and must not be when it has
// not. The rule applies to what this node emits, whose addresses and ports are its own.
TEST(BuiltinMediaEngineTest, ARepeatedOfferKeepsTheVersionItWasGiven) {
  auto engine = make_engine(24600, 24620);
  auto call = make_call();

  Flags flags;
  flags.participant = 0;

  const auto first = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(first.ok) << first.error;

  const auto again = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(again.ok) << again.error;

  SDP one;
  SDP two;
  ASSERT_TRUE(one.parse(first.sdp));
  ASSERT_TRUE(two.parse(again.sdp));

  // The same stream and relay ports: nothing changed.
  EXPECT_EQ(one.origin().sessionVersion, two.origin().sessionVersion);
  EXPECT_EQ(first.sdp, again.sdp);

  engine->close();
}

TEST(BuiltinMediaEngineTest, AnOfferThatChangesTheDescriptionCarriesAHigherVersion) {
  auto engine = make_engine(24630, 24650);
  auto call = make_call();

  Flags flags;
  flags.participant = 0;

  const auto first = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(first.ok) << first.error;

  // A re-offer that adds video changes the description.
  const std::string with_video = std::string(kOffer) + "m=video 51372 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n";

  const auto second = engine->offer(call, with_video, flags);
  ASSERT_TRUE(second.ok) << second.error;

  SDP one;
  SDP two;
  ASSERT_TRUE(one.parse(first.sdp));
  ASSERT_TRUE(two.parse(second.sdp));

  EXPECT_GT(std::stoull(two.origin().sessionVersion), std::stoull(one.origin().sessionVersion));

  engine->close();
}

// The first description keeps the endpoint's own version.
TEST(BuiltinMediaEngineTest, TheFirstDescriptionKeepsTheEndpointsVersion) {
  auto engine = make_engine(24660, 24680);
  auto call = make_call();

  Flags flags;
  flags.participant = 0;

  const auto first = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(first.ok) << first.error;

  SDP produced;
  ASSERT_TRUE(produced.parse(first.sdp));

  SDP given;
  ASSERT_TRUE(given.parse(kOffer));

  EXPECT_EQ(produced.origin().sessionVersion, given.origin().sessionVersion);

  engine->close();
}

// Each leg's description has its own version counter.
TEST(BuiltinMediaEngineTest, EachLegHasAVersionOfItsOwn) {
  auto engine = make_engine(24690, 24710);
  auto call = make_call();

  Flags caller;
  caller.participant = 0;

  Flags callee;
  callee.participant = 1;

  ASSERT_TRUE(engine->offer(call, kOffer, caller).ok);
  const auto answered = engine->answer(call, kAnswer, callee);
  ASSERT_TRUE(answered.ok) << answered.error;

  SDP produced;
  ASSERT_TRUE(produced.parse(answered.sdp));

  SDP given;
  ASSERT_TRUE(given.parse(kAnswer));

  // Bob's first description, so still Bob's version, not one carried over from Alice's leg.
  EXPECT_EQ(produced.origin().sessionVersion, given.origin().sessionVersion);

  engine->close();
}

TEST(BuiltinMediaEngineTest, QueryReportsHowLongTheMediaHasBeenSilent) {
  auto engine = make_engine(24000, 24040);
  auto call = make_call();

  // A call the engine holds nothing for reports null, not a number.
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":null"), std::string::npos);

  ASSERT_TRUE(engine->offer(call, kOffer, Flags{}).ok);

  // Idle is measured from allocation, so a call whose media has not begun reads as young.
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":0"), std::string::npos);

  engine->release(call);
  EXPECT_NE(engine->query(call).find("\"idle_seconds\":null"), std::string::npos);
}

// A packet arriving resets the reading, and the figure is the shortest idle of all the call's relays. RTCP
// counts too, which keeps a call on hold or with silence suppression alive (RFC 3550 6).
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

  // The relay reads on the global io_context thread, so the store lands a moment after the send returns.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool seen = false;

  while (std::chrono::steady_clock::now() < deadline && !seen) {
    seen = engine->query(call).find("\"idle_seconds\":0") != std::string::npos;
    if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(seen) << "query said " << engine->query(call);

  engine->release(call);
}

// A public address may be a name, as behind dynamic DNS. The engine resolves it and writes the address in
// c=, because phones do not resolve a name there.
TEST(BuiltinMediaEngineTest, APublicAddressThatIsANameIsWrittenAsTheAddressItResolvesTo) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=localhost&port_min=26100&port_max=26120");
  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));
  engine->connect();

  auto call = make_call();
  Flags flags;
  flags.participant = 0;

  const auto offered = engine->offer(call, kOffer, flags);
  ASSERT_TRUE(offered.ok) << offered.error;

  SDP sdp;
  ASSERT_TRUE(sdp.parse(offered.sdp));
  EXPECT_EQ(sdp.connection().address, "127.0.0.1") << offered.sdp;
  EXPECT_EQ(offered.sdp.find("localhost"), std::string::npos) << offered.sdp;

  engine->release(call);
  engine->close();
}

// A name that does not resolve is not written: the engine declines and the description travels on unchanged.
TEST(BuiltinMediaEngineTest, APublicNameThatDoesNotResolveIsDeclinedRatherThanWritten) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=nowhere.invalid&port_min=26130&port_max=26150");
  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));
  engine->connect();

  auto call = make_call();
  Flags flags;
  flags.participant = 0;

  const auto offered = engine->offer(call, kOffer, flags);
  EXPECT_FALSE(offered.ok);
  EXPECT_NE(offered.error.find("nowhere.invalid"), std::string::npos) << offered.error;

  engine->close();
}
