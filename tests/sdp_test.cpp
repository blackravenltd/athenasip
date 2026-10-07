//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <string>

#include "sdp.h"

using namespace athenasip;

namespace {

// RFC 8866 5: a minimal session description.
const char* kMinimal =
    "v=0\r\n"
    "o=alice 2890844526 2890844526 IN IP4 198.51.100.1\r\n"
    "s=-\r\n"
    "c=IN IP4 198.51.100.1\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0\r\n"
    "a=rtpmap:0 PCMU/8000\r\n";

// A browser's offer: BUNDLE, rtcp-mux, ICE and DTLS, two media sections.
const char* kWebRtc =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE audio video\r\n"
    "a=msid-semantic: WMS stream\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 103\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlZ6\r\n"
    "a=fingerprint:sha-256 4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B\r\n"
    "a=setup:actpass\r\n"
    "a=mid:audio\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 100\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:video\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:100 VP8/90000\r\n";

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

}  // namespace

TEST(SDPTest, ParsesAMinimalSession) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kMinimal));
  EXPECT_TRUE(sdp.is_valid());
}

// RFC 8866 5: v=, o=, s= and t= are mandatory; a description missing one is rejected.
TEST(SDPTest, RejectsAMissingMandatoryField) {
  SDP no_version;
  EXPECT_FALSE(no_version.parse(
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"));

  SDP no_origin;
  EXPECT_FALSE(no_origin.parse(
      "v=0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"));

  SDP no_timing;
  EXPECT_FALSE(no_timing.parse(
      "v=0\r\n"
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"));
}

TEST(SDPTest, RejectsRubbish) {
  SDP sdp;
  EXPECT_FALSE(sdp.parse("this is not an SDP at all\r\n"));
  EXPECT_FALSE(sdp.parse(""));
}

// Every line round-trips unchanged; dropping one would break ICE, DTLS or BUNDLE.
TEST(SDPTest, RoundTripsEveryLineOfARealOffer) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kWebRtc));

  const auto out = sdp.to_string();

  for (const auto& line : {"a=group:BUNDLE audio video", "a=msid-semantic: WMS stream", "a=ice-ufrag:4ZcD",
                           "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlZ6", "a=fingerprint:sha-256 4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B",
                           "a=setup:actpass", "a=mid:audio", "a=rtcp-mux", "a=rtpmap:111 opus/48000/2", "a=mid:video",
                           "a=rtpmap:100 VP8/90000"}) {
    EXPECT_TRUE(contains(out, line)) << "lost: " << line;
  }
}

TEST(SDPTest, RoundTripIsByteIdenticalWhenNothingIsChanged) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kWebRtc));
  EXPECT_EQ(sdp.to_string(), std::string(kWebRtc));
}

// RFC 8866 5.13: unknown line types are preserved, not deleted.
TEST(SDPTest, PreservesUnknownLineTypes) {
  const std::string with_unknown =
      "v=0\r\n"
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "x=some-future-extension\r\n"
      "m=audio 49170 RTP/AVP 0\r\n"
      "y=another-one\r\n"
      "a=rtpmap:0 PCMU/8000\r\n";

  SDP sdp;
  ASSERT_TRUE(sdp.parse(with_unknown));

  const auto out = sdp.to_string();
  EXPECT_TRUE(contains(out, "x=some-future-extension"));
  EXPECT_TRUE(contains(out, "y=another-one"));
}

// RFC 8866 5.9: there may be more than one time description.
TEST(SDPTest, PreservesMultipleTimeDescriptions) {
  const std::string two_times =
      "v=0\r\n"
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "t=3034423619 3042462419\r\n"
      "r=604800 3600 0 90000\r\n"
      "t=3034423619 3042462419\r\n"
      "m=audio 49170 RTP/AVP 0\r\n";

  SDP sdp;
  ASSERT_TRUE(sdp.parse(two_times));

  const auto out = sdp.to_string();
  EXPECT_EQ(out.find("t=3034423619"), out.rfind("t=3034423619") - (out.rfind("t=3034423619") - out.find("t=3034423619")));

  // Both time lines and the repeat survive.
  std::size_t count = 0;
  for (std::size_t at = out.find("t=3034423619"); at != std::string::npos; at = out.find("t=3034423619", at + 1)) count++;
  EXPECT_EQ(count, 2u);
  EXPECT_TRUE(contains(out, "r=604800 3600 0 90000"));
}

// RFC 8866 5.14: the port may carry a count, "m=<media> <port>/<n> <proto>".
TEST(SDPTest, ParsesAPortCount) {
  const std::string with_count =
      "v=0\r\n"
      "o=alice 1 1 IN IP4 198.51.100.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=video 49170/2 RTP/AVP 31\r\n";

  SDP sdp;
  ASSERT_TRUE(sdp.parse(with_count));
  ASSERT_EQ(sdp.media().size(), 1u);

  EXPECT_EQ(sdp.media()[0].description.media, "video");
  EXPECT_EQ(sdp.media()[0].description.port, 49170);
  EXPECT_EQ(sdp.media()[0].description.port_count, 2u);
  EXPECT_EQ(sdp.media()[0].description.proto, "RTP/AVP");
  ASSERT_EQ(sdp.media()[0].description.formats.size(), 1u);
  EXPECT_EQ(sdp.media()[0].description.formats[0], "31");

  EXPECT_TRUE(contains(sdp.to_string(), "m=video 49170/2 RTP/AVP 31"));
}

TEST(SDPTest, SeparatesMediaSections) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kWebRtc));

  ASSERT_EQ(sdp.media().size(), 2u);
  EXPECT_EQ(sdp.media()[0].description.media, "audio");
  EXPECT_EQ(sdp.media()[1].description.media, "video");

  // Media attributes belong to their own section.
  EXPECT_EQ(sdp.media()[0].mid(), "audio");
  EXPECT_EQ(sdp.media()[1].mid(), "video");
  EXPECT_TRUE(contains(sdp.media()[0].attributes()[0], "rtcp:9"));
}

// Rewriting the connection address and port leaves every other line unchanged.
TEST(SDPTest, RewritingConnectionAndPortLeavesTheRestAlone) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kWebRtc));

  ConnectionInfo relay;
  relay.nettype = "IN";
  relay.addrtype = "IP4";
  relay.address = "203.0.113.5";

  sdp.set_connection(relay);
  sdp.media()[0].set_connection(relay);
  sdp.media()[0].description.port = 22000;

  const auto out = sdp.to_string();

  EXPECT_TRUE(contains(out, "m=audio 22000 UDP/TLS/RTP/SAVPF 111 103"));
  EXPECT_TRUE(contains(out, "c=IN IP4 203.0.113.5"));

  EXPECT_TRUE(contains(out, "a=ice-ufrag:4ZcD"));
  EXPECT_TRUE(contains(out, "a=fingerprint:sha-256 4A:AD:B9:B1:3F:82:18:3B:54:02:12:DF:3E:5D:49:6B"));
  EXPECT_TRUE(contains(out, "a=group:BUNDLE audio video"));
  EXPECT_TRUE(contains(out, "a=rtcp-mux"));
  EXPECT_TRUE(contains(out, "m=video 9 UDP/TLS/RTP/SAVPF 100"));
}

TEST(SDPTest, SessionNameIsNeverEmittedEmpty) {
  SDP sdp;
  ASSERT_TRUE(sdp.parse(kMinimal));

  // RFC 8866 5.3: s= must have at least one character; "-" is the conventional filler.
  EXPECT_FALSE(contains(sdp.to_string(), "s=\r\n"));
}
