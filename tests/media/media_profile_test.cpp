//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

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

struct ProfileFixture : ProxyFixture {
  std::shared_ptr<RecordingMediaEngine> engine = std::make_shared<RecordingMediaEngine>();

  explicit ProfileFixture(const std::string& callee_transport) {
    engine->connect(core->strand(), [](plugins::Status) {});
    core->media_register(engine);
    settle();

    // Bob answers over the transport under test, which is what says whether he is a
    // browser or a phone.
    callee = make_channel("192.0.2.20", &callee_connection, callee_transport);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), callee, 3600);
  }

  std::string invite_with_body() {
    auto raw = invite();
    raw.insert(raw.size() - 2, "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(kOffer.size()) + "\r\n");
    return raw + kOffer;
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
  ProfileFixture f("udp");

  // Alice is the browser this time, so the answer going back to her needs WebRTC even
  // though the callee is a phone.
  f.caller = f.make_channel("192.0.2.10", &f.caller_connection, "wss");

  f.receive(f.caller, f.invite_with_body());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  std::string ok = "SIP/2.0 200 OK\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) ok += "Via: " + via->to_string() + "\r\n";
  ok += "From: <sip:alice@example.com>;tag=alice\r\n";
  ok += "To: <sip:bob@example.com>;tag=bob\r\n";
  ok += "Call-ID: call-proxy\r\n";
  ok += "CSeq: 1 INVITE\r\n";
  ok += "Contact: <sip:bob@192.0.2.20:5060>\r\n";
  ok += "Content-Type: application/sdp\r\n";
  ok += "Content-Length: " + std::to_string(kOffer.size()) + "\r\n";
  ok += "\r\n";
  ok += kOffer;

  f.receive(f.callee, ok);
  f.settle();

  const auto calls = f.engine->calls();
  ASSERT_GE(calls.size(), 2u);

  const auto& answer = calls.back();
  EXPECT_FALSE(answer.was_offer);
  EXPECT_EQ(answer.flags.target, Flags::Profile::WebRtc);
}
