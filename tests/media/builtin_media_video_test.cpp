//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../helpers/sync_media_engine_helper.h"
#include "../mocks/logger_mock.h"
#include "call.h"
#include "media/builtin_media_engine.h"
#include "media/media_engine.h"
#include "sdp.h"

using namespace athenasip;
using athenasip::media::BuiltinMediaEngine;
using athenasip::media::Flags;

// RFC 3264 6: an answer has one m-line per offered stream, in order; each stream has its own port, and a
// declined stream is answered with port zero in its place. A relay does this for every stream.
namespace {

using udp = boost::asio::ip::udp;

std::shared_ptr<SyncMediaEngine> make_engine(std::uint16_t port_min, std::uint16_t port_max) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=127.0.0.1&port_min=" + std::to_string(port_min) +
                                          "&port_max=" + std::to_string(port_max));

  auto engine = std::make_shared<SyncMediaEngine>(std::make_shared<BuiltinMediaEngine>(logger, url));
  engine->connect();
  return engine;
}

std::shared_ptr<Call> make_call() {
  auto call = std::make_shared<Call>();
  call->id = "call-video-1";
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  return call;
}

// Audio and video from one end, at two ports. A video port of zero is the stream declined.
std::string description(const std::string& who, std::uint16_t audio, std::uint16_t video) {
  return "v=0\r\no=" + who + " 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n" + "m=audio " + std::to_string(audio) +
         " RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n" + "m=video " + std::to_string(video) + " RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n";
}

std::string datagram(udp::socket& socket, std::chrono::milliseconds bound = std::chrono::milliseconds(500)) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (socket.available() > 0) {
      char buffer[64] = {};
      udp::endpoint from;
      const auto bytes = socket.receive_from(boost::asio::buffer(buffer), from);
      return std::string(buffer, bytes);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::string();
}

// Two ends, each with an audio and a video socket, and a call set up between them through the relay.
struct VideoCall {
  boost::asio::io_context io;
  udp::socket caller_audio{io, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)};
  udp::socket caller_video{io, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)};
  udp::socket callee_audio{io, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)};
  udp::socket callee_video{io, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)};

  std::shared_ptr<SyncMediaEngine> engine;
  std::shared_ptr<Call> call = make_call();

  SDP to_callee;
  SDP to_caller;

  explicit VideoCall(std::uint16_t port_min) : engine(make_engine(port_min, port_min + 60)) {}

  ~VideoCall() {
    engine->release(call);
    engine->close();
  }

  static udp::endpoint relay(const SDP& sdp, std::size_t stream) {
    return udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), sdp.media()[stream].description.port);
  }
};

}  // namespace

// Each offered stream gets a relay port of its own, in the offer's order.
TEST(BuiltinMediaVideoTest, EachStreamOfAnOfferGetsARelayPortOfItsOwn) {
  VideoCall v(25000);

  Flags from_caller;
  from_caller.participant = 0;

  const auto offered = v.engine->offer(v.call, description("alice", 40000, 40002), from_caller);
  ASSERT_TRUE(offered.ok) << offered.error;
  ASSERT_TRUE(v.to_callee.parse(offered.sdp));

  ASSERT_EQ(v.to_callee.media().size(), 2u);
  EXPECT_EQ(v.to_callee.media()[0].description.media, "audio");
  EXPECT_EQ(v.to_callee.media()[1].description.media, "video");

  const auto audio = v.to_callee.media()[0].description.port;
  const auto video = v.to_callee.media()[1].description.port;

  EXPECT_NE(audio, 0);
  EXPECT_NE(video, 0);
  EXPECT_NE(audio, video);
  EXPECT_NE(audio, 40000) << "the callee is told the relay, not the caller";
  EXPECT_NE(video, 40002);
}

// Video crosses both ways on its own ports and nowhere else.
TEST(BuiltinMediaVideoTest, VideoIsRelayedBothWaysBesideTheAudio) {
  VideoCall v(25100);

  Flags from_caller;
  from_caller.participant = 0;
  Flags from_callee;
  from_callee.participant = 1;

  const auto offered =
      v.engine->offer(v.call, description("alice", v.caller_audio.local_endpoint().port(), v.caller_video.local_endpoint().port()), from_caller);
  ASSERT_TRUE(offered.ok) << offered.error;

  const auto answered =
      v.engine->answer(v.call, description("bob", v.callee_audio.local_endpoint().port(), v.callee_video.local_endpoint().port()), from_callee);
  ASSERT_TRUE(answered.ok) << answered.error;

  ASSERT_TRUE(v.to_callee.parse(offered.sdp));
  ASSERT_TRUE(v.to_caller.parse(answered.sdp));
  ASSERT_EQ(v.to_caller.media().size(), 2u);

  // Each end sends to the port the description it was handed names for that stream.
  v.caller_video.send_to(boost::asio::buffer("video-from-caller", 17), VideoCall::relay(v.to_caller, 1));
  EXPECT_EQ(datagram(v.callee_video), "video-from-caller");

  v.callee_video.send_to(boost::asio::buffer("video-from-callee", 17), VideoCall::relay(v.to_callee, 1));
  EXPECT_EQ(datagram(v.caller_video), "video-from-callee");

  v.caller_audio.send_to(boost::asio::buffer("audio-from-caller", 17), VideoCall::relay(v.to_caller, 0));
  EXPECT_EQ(datagram(v.callee_audio), "audio-from-caller");

  // Nothing leaked across: the audio sockets saw no video, and the reverse.
  EXPECT_EQ(datagram(v.callee_audio, std::chrono::milliseconds(100)), "");
  EXPECT_EQ(datagram(v.callee_video, std::chrono::milliseconds(100)), "");
}

// RFC 3264 6: a declined stream is answered with port zero in its place, and gets no relay port.
TEST(BuiltinMediaVideoTest, ADeclinedVideoStreamStaysDeclinedAndTheAudioStillCrosses) {
  VideoCall v(25200);

  Flags from_caller;
  from_caller.participant = 0;
  Flags from_callee;
  from_callee.participant = 1;

  const auto offered =
      v.engine->offer(v.call, description("alice", v.caller_audio.local_endpoint().port(), v.caller_video.local_endpoint().port()), from_caller);
  ASSERT_TRUE(offered.ok) << offered.error;

  const auto answered = v.engine->answer(v.call, description("bob", v.callee_audio.local_endpoint().port(), 0), from_callee);
  ASSERT_TRUE(answered.ok) << answered.error;

  ASSERT_TRUE(v.to_caller.parse(answered.sdp));
  ASSERT_EQ(v.to_caller.media().size(), 2u) << "one m-line for each the offer had";
  EXPECT_EQ(v.to_caller.media()[1].description.media, "video");
  EXPECT_EQ(v.to_caller.media()[1].description.port, 0) << "declined is port zero, not a relay port";
  EXPECT_NE(v.to_caller.media()[0].description.port, 0);

  v.caller_audio.send_to(boost::asio::buffer("audio-from-caller", 17), VideoCall::relay(v.to_caller, 0));
  EXPECT_EQ(datagram(v.callee_audio), "audio-from-caller");
}

// Adding video by re-INVITE leaves the audio relay port where it was and gives the video its own.
TEST(BuiltinMediaVideoTest, AddingVideoToACallLeavesItsAudioWhereItWas) {
  VideoCall v(25300);

  Flags from_caller;
  from_caller.participant = 0;
  Flags from_callee;
  from_callee.participant = 1;

  const std::string audio_only = "v=0\r\no=alice 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio " +
                                 std::to_string(v.caller_audio.local_endpoint().port()) + " RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";

  const auto first = v.engine->offer(v.call, audio_only, from_caller);
  ASSERT_TRUE(first.ok) << first.error;

  SDP before;
  ASSERT_TRUE(before.parse(first.sdp));
  ASSERT_EQ(before.media().size(), 1u);

  const auto second =
      v.engine->offer(v.call, description("alice", v.caller_audio.local_endpoint().port(), v.caller_video.local_endpoint().port()), from_caller);
  ASSERT_TRUE(second.ok) << second.error;

  SDP after;
  ASSERT_TRUE(after.parse(second.sdp));
  ASSERT_EQ(after.media().size(), 2u);

  EXPECT_EQ(after.media()[0].description.port, before.media()[0].description.port);
  EXPECT_NE(after.media()[1].description.port, 0);
  EXPECT_NE(after.media()[1].description.port, after.media()[0].description.port);
}
