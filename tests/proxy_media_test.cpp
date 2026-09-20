//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "media/builtin_media_engine.h"
#include "sdp.h"

#include "helpers/proxy_fixture_helper.h"
#include "helpers/sync_media_engine_helper.h"

using namespace athenasip;

namespace {

constexpr std::uint16_t kPortMin = 24100;
constexpr std::uint16_t kPortMax = 24200;
const char* kPublicAddress = "203.0.113.5";

// A node whose media engine is the builtin relay, driven the way a transport drives it.
// RFC 3264 is an end-to-end exchange, so the fixture plays both ends: Alice offers in
// the INVITE and Bob answers in the 200, and what each of them is handed has to name
// this node rather than the other end.
struct MediaFixture : ProxyFixture {
  std::shared_ptr<SyncMediaEngine> engine;

  MediaFixture() {
    auto url = std::make_shared<types::URL>("builtin://?bind_address=127.0.0.1&public_address=" + std::string(kPublicAddress) +
                                            "&port_min=" + std::to_string(kPortMin) + "&port_max=" + std::to_string(kPortMax));

    engine = std::make_shared<SyncMediaEngine>(std::make_shared<media::BuiltinMediaEngine>(logger, url));
    engine->connect();

    core->media_register(engine->driver());
    bind_bob();
  }

  ~MediaFixture() {
    settle();
    engine->close();
  }

  // Alice's offer: plain RTP, one audio stream, pointed at Alice's own address.
  static std::string offer() {
    return "v=0\r\n"
           "o=alice 2890844526 2890844526 IN IP4 192.0.2.10\r\n"
           "s=-\r\n"
           "c=IN IP4 192.0.2.10\r\n"
           "t=0 0\r\n"
           "m=audio 49170 RTP/AVP 0\r\n"
           "a=rtpmap:0 PCMU/8000\r\n"
           "a=rtcp:49171\r\n";
  }

  // Bob's answer, same stream, pointed at Bob's own address.
  static std::string answer() {
    return "v=0\r\n"
           "o=bob 2890844600 2890844600 IN IP4 192.0.2.20\r\n"
           "s=-\r\n"
           "c=IN IP4 192.0.2.20\r\n"
           "t=0 0\r\n"
           "m=audio 6000 RTP/AVP 0\r\n"
           "a=rtpmap:0 PCMU/8000\r\n"
           "a=rtcp:6001\r\n";
  }

  // A WebRTC offer, which the builtin relay cannot take: it is plain RTP only.
  static std::string webrtc_offer() {
    return "v=0\r\n"
           "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=rtpmap:111 opus/48000/2\r\n"
           "a=ice-ufrag:4ZcD\r\n"
           "a=fingerprint:sha-256 AA:BB:CC\r\n"
           "a=rtcp-mux\r\n";
  }

  // The INVITE as Alice sends it, with a session description in it.
  std::string invite_with(const std::string& sdp) {
    std::string raw = "INVITE sip:bob@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-invite\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "Content-Type: application/sdp\r\n";
    raw += "Content-Length: " + std::to_string(sdp.size()) + "\r\n";
    raw += "\r\n";
    raw += sdp;
    return raw;
  }

  // Bob's 200, carrying his answer back up the Via chain the proxy built.
  std::string ok_with(const std::string& sdp) {
    return response_from_callee(200, "OK", "bob", "sip:bob@192.0.2.20:5060",
                                "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n") +
           sdp;
  }

  std::string bye() {
    auto forwarded = request_with(callee_connection, "INVITE");
    std::string route;
    if (forwarded) {
      for (const auto& value : forwarded->header->headers_map["Record-Route"]) route += "Route: " + value->to_string() + "\r\n";
    }

    std::string raw = "BYE sip:alice@192.0.2.10:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=z9hG4bK-bye\r\n";
    raw += route;
    raw += "From: <sip:bob@example.com>;tag=bob\r\n";
    raw += "To: <sip:alice@example.com>;tag=alice\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 BYE\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }

  // The session description in the first message of this kind, parsed.
  static SDP sdp_of(const std::shared_ptr<athenasip::SIPMessage>& message) {
    SDP sdp;
    if (message) sdp.parse(message->body);
    return sdp;
  }
};

bool in_port_range(std::uint16_t port) { return port >= kPortMin && port < kPortMax; }

// Where the far end is told to send: the media-level connection when the description
// carries one, and the session-level one when it does not (RFC 8866 section 5.7). The
// o= line is an identifier for the session and not an address to send to, which is why
// it is not rewritten and not read here.
std::string media_address(const SDP& sdp, std::size_t index) {
  const auto& media = sdp.media()[index];
  return media.has_connection() ? media.connection().address : sdp.connection().address;
}

}  // namespace

// The whole point of anchoring: what Bob is told to send to is this node, not Alice. A
// proxy that passed the offer through untouched would have the two endpoints send media
// straight to each other, which is exactly what a node that has to bridge, record or
// traverse NAT cannot allow.
TEST(ProxyMediaTest, TheOfferTheCalleeReceivesNamesThisNode) {
  MediaFixture fixture;

  fixture.receive(fixture.caller, fixture.invite_with(MediaFixture::offer()));

  auto forwarded = ProxyFixture::request_with(fixture.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  auto sdp = MediaFixture::sdp_of(forwarded);
  ASSERT_TRUE(sdp.is_valid()) << forwarded->body;
  ASSERT_EQ(sdp.media().size(), 1u);

  EXPECT_EQ(media_address(sdp, 0), kPublicAddress);
  EXPECT_TRUE(in_port_range(sdp.media()[0].description.port)) << sdp.media()[0].description.port;
}

// And the other half of RFC 3264: the answer Alice gets back has to name this node too,
// or she sends her media to Bob and only one direction is anchored.
TEST(ProxyMediaTest, TheAnswerTheCallerReceivesNamesThisNode) {
  MediaFixture fixture;

  fixture.receive(fixture.caller, fixture.invite_with(MediaFixture::offer()));
  fixture.receive(fixture.callee, fixture.ok_with(MediaFixture::answer()));

  auto upstream = ProxyFixture::response_with(fixture.caller_connection, 200);
  ASSERT_NE(upstream, nullptr);

  auto sdp = MediaFixture::sdp_of(upstream);
  ASSERT_TRUE(sdp.is_valid()) << upstream->body;
  ASSERT_EQ(sdp.media().size(), 1u);

  EXPECT_EQ(media_address(sdp, 0), kPublicAddress);
  EXPECT_TRUE(in_port_range(sdp.media()[0].description.port)) << sdp.media()[0].description.port;
}

// RFC 3261 16.6: a proxy does not touch a body. Anchoring is a deliberate departure and
// it applies to a session description; anything else goes through as it arrived.
TEST(ProxyMediaTest, AMessageWithNoSessionDescriptionIsUntouched) {
  MediaFixture fixture;

  fixture.receive(fixture.caller, fixture.invite());

  auto forwarded = ProxyFixture::request_with(fixture.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_TRUE(forwarded->body.empty());
}

// The builtin relay is plain RTP and says so. A browser's offer must not be quietly
// rewritten into something that cannot work - the ICE candidates and the DTLS
// fingerprint belong to the endpoint that wrote them - so it travels on as it arrived
// and the call is not failed over it.
TEST(ProxyMediaTest, AnOfferTheEngineCannotTakeTravelsOnUnchanged) {
  MediaFixture fixture;

  const auto offer = MediaFixture::webrtc_offer();
  fixture.receive(fixture.caller, fixture.invite_with(offer));

  auto forwarded = ProxyFixture::request_with(fixture.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->body, offer);
}

// A node that anchors media has to give the ports back, and the only thing that says a
// call is over is the dialog ending. This is why the node tracks dialogs at all.
TEST(ProxyMediaTest, TheMediaIsReleasedWhenTheCallEnds) {
  MediaFixture fixture;

  fixture.receive(fixture.caller, fixture.invite_with(MediaFixture::offer()));
  fixture.receive(fixture.callee, fixture.ok_with(MediaFixture::answer()));

  auto call = fixture.call();
  ASSERT_NE(call, nullptr);
  EXPECT_NE(fixture.engine->query(call).find("\"relay_sets\":2"), std::string::npos);

  fixture.receive(fixture.callee, fixture.bye());
  fixture.settle();

  EXPECT_NE(fixture.engine->query(call).find("\"relay_sets\":0"), std::string::npos);
}

// One bridge, not two sinks. Both legs are pointed at the same relay for the same
// stream, which is what makes what Alice sends come out at Bob.
TEST(ProxyMediaTest, BothLegsAreAnchoredOnTheSameStream) {
  MediaFixture fixture;

  fixture.receive(fixture.caller, fixture.invite_with(MediaFixture::offer()));
  fixture.receive(fixture.callee, fixture.ok_with(MediaFixture::answer()));

  auto call = fixture.call();
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(call->participants.size(), 2u);

  ASSERT_EQ(call->participants[0].streams.size(), 1u);
  ASSERT_EQ(call->participants[1].streams.size(), 1u);

  // Two relay sets for the call - one RTP, one RTCP - and not four.
  EXPECT_NE(fixture.engine->query(call).find("\"relay_sets\":2"), std::string::npos);
}
