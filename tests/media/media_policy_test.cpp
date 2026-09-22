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
#include "types/realm.h"

using namespace athenasip;
using athenasip::media::Flags;
using athenasip::types::MediaPolicy;

namespace {

// The same recording engine the profile tests use: what is asserted here is what the
// node asked an engine for, and whether it asked at all.
class RecordingMediaEngine : public media::MediaEngine {
 public:
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
    _record(flags);

    // Rewritten, so a test can tell an anchored description from one that went past
    // untouched.
    _complete(std::move(on), std::move(handler), media::Result::success(std::string("v=0\r\no=anchored 1 1 IN IP4 203.0.113.5\r\ns=-\r\nt=0 0\r\n")));
  }

  void answer(plugins::Executor on, std::shared_ptr<athenasip::Call> call, std::string sdp, Flags flags, MediaHandler handler) override {
    (void)call;
    _record(flags);
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

  std::vector<Flags> seen() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _seen;
  }

 private:
  void _record(const Flags& flags) {
    std::lock_guard<std::mutex> lock(_mutex);
    _seen.push_back(flags);
  }

  mutable std::mutex _mutex;
  std::vector<Flags> _seen;
  bool _connected = false;
};

const std::string kOffer =
    "v=0\r\n"
    "o=alice 1 1 IN IP4 192.0.2.10\r\n"
    "s=-\r\n"
    "c=IN IP4 192.0.2.10\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0\r\n";

struct PolicyFixture : ProxyFixture {
  std::shared_ptr<RecordingMediaEngine> engine = std::make_shared<RecordingMediaEngine>();

  PolicyFixture(const MediaPolicy& policy, const std::string& callee_transport) {
    engine->connect(core->strand(), [](plugins::Status) {});
    core->media_register(engine);
    settle();

    auto realm = store->realm_get_by_name("example.com");
    realm->media = policy;
    store->realm_update(realm);

    callee = make_channel("192.0.2.20", &callee_connection, callee_transport);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), callee, 3600);
  }

  std::string invite_with_body(const std::string& branch = "z9hG4bK-invite") {
    auto raw = invite(branch);
    raw.insert(raw.size() - 2, "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(kOffer.size()) + "\r\n");
    return raw + kOffer;
  }
};

MediaPolicy with_profiles(MediaPolicy::Profiles profiles) {
  MediaPolicy policy;
  policy.profiles = profiles;
  return policy;
}

}  // namespace

// The default is what a node did before a realm could say anything: anchor, and read
// each leg from its transport.
TEST(MediaPolicyTest, TheDefaultIsToAnchorAndReadTheLegFromItsTransport) {
  const MediaPolicy fresh;

  EXPECT_TRUE(fresh.anchor);
  EXPECT_EQ(fresh.profiles, MediaPolicy::Profiles::FromTransport);
}

// A realm with a typo in its policy keeps doing what it was doing. Changing what a
// node does to media because a word was misspelled is the worse failure.
TEST(MediaPolicyTest, AnUnreadableProfileNameLeavesThePolicyAlone) {
  EXPECT_EQ(MediaPolicy::profiles_from_string("webrtc", MediaPolicy::Profiles::Mirror), MediaPolicy::Profiles::WebRtc);
  EXPECT_EQ(MediaPolicy::profiles_from_string("  RTP  ", MediaPolicy::Profiles::Mirror), MediaPolicy::Profiles::PlainRtp);

  EXPECT_EQ(MediaPolicy::profiles_from_string("web-rtc", MediaPolicy::Profiles::Mirror), MediaPolicy::Profiles::Mirror);
  EXPECT_EQ(MediaPolicy::profiles_from_string("", MediaPolicy::Profiles::WebRtc), MediaPolicy::Profiles::WebRtc);
}

TEST(MediaPolicyTest, EveryProfileNameSurvivesBeingWrittenDownAndReadBack) {
  for (const auto profiles : {MediaPolicy::Profiles::FromTransport, MediaPolicy::Profiles::Mirror, MediaPolicy::Profiles::PlainRtp,
                              MediaPolicy::Profiles::WebRtc, MediaPolicy::Profiles::SrtpSdes}) {
    EXPECT_EQ(MediaPolicy::profiles_from_string(MediaPolicy::to_string(profiles)), profiles);
  }
}

// RFC 7118 is SIP over WebSocket and requires no WebRTC, so a realm whose WebSocket
// clients are phones has to be able to say so. Without this, the transport rule offers
// them ICE and DTLS they cannot answer.
TEST(MediaPolicyTest, ARealmCanSayItsWebSocketClientsArePhones) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::PlainRtp), "wss");

  f.receive(f.caller, f.invite_with_body());

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::PlainRtp);
}

TEST(MediaPolicyTest, ARealmCanSayEveryLegIsWebRtc) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::WebRtc), "udp");

  f.receive(f.caller, f.invite_with_body());

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::WebRtc);
}

// Nothing about a flow distinguishes a desk phone that wants SRTP from one that does
// not, so this profile can only be asked for.
TEST(MediaPolicyTest, ARealmCanAskForSrtpWithTheKeysInTheDescription) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::SrtpSdes), "udp");

  f.receive(f.caller, f.invite_with_body());

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::SrtpSdes);
}

TEST(MediaPolicyTest, ARealmCanLeaveItToTheEngine) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::Mirror), "wss");

  f.receive(f.caller, f.invite_with_body());

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::Mirror);
}

// Not anchoring is the same outcome as having no engine: the description travels
// exactly as it arrived. The difference is that somebody chose it.
TEST(MediaPolicyTest, ARealmThatDoesNotAnchorLeavesTheDescriptionAlone) {
  MediaPolicy policy;
  policy.anchor = false;

  PolicyFixture f(policy, "udp");

  f.receive(f.caller, f.invite_with_body());

  EXPECT_TRUE(f.engine->seen().empty());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->body, kOffer);
}

TEST(MediaPolicyTest, ARealmThatAnchorsHandsOnWhatTheEngineProduced) {
  PolicyFixture f(MediaPolicy{}, "udp");

  f.receive(f.caller, f.invite_with_body());

  ASSERT_FALSE(f.engine->seen().empty());

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_NE(forwarded->body.find("o=anchored"), std::string::npos) << forwarded->body;
}

// A re-INVITE travels in-dialog on its route set and never looks a realm up, so a
// policy read afresh for each message would have hold and resume behave differently
// from the INVITE that started the call. It is decided once and kept on the call.
TEST(MediaPolicyTest, TheCallKeepsThePolicyForTheRequestsThatFollow) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::WebRtc), "udp");

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

  const auto before = f.engine->seen().size();
  ASSERT_GE(before, 2u);

  // The answer went back to a udp leg, and the realm still says WebRTC.
  EXPECT_EQ(f.engine->seen().back().target, Flags::Profile::WebRtc);
}
