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

// A recording engine: what is asserted is what the node asked an engine for, and whether it asked at all.
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

    // Rewritten, so a test can tell an anchored description from an untouched one.
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

    // Set on the realm, so it overrides whatever the server's default is.
    auto realm = store->realm_get_by_name("example.com");
    realm->behaviour.media_anchor = policy.anchor;
    realm->behaviour.media_profile = policy.profiles;
    store->realm_update(realm);

    callee = make_channel("192.0.2.20", &callee_connection, callee_transport);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), callee, 3600);
  }

  std::string invite_with_body(const std::string& branch = "z9hG4bK-invite") { return with_body(invite(branch)); }

  std::string ok_with_body() { return with_body(response_from_callee(200, "OK")); }

  static std::string with_body(std::string raw) {
    if (raw.empty()) return raw;
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

// A realm that sets no behaviour follows the server's default, and the shipped one passes the caller's
// profile through: a browser's offer goes on as WebRTC whatever the callee's transport.
struct DefaultFixture : ProxyFixture {
  std::shared_ptr<RecordingMediaEngine> engine = std::make_shared<RecordingMediaEngine>();

  explicit DefaultFixture(const std::string& callee_transport) {
    engine->connect(core->strand(), [](plugins::Status) {});
    core->media_register(engine);
    settle();

    callee = make_channel("192.0.2.20", &callee_connection, callee_transport);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), callee, 3600);
  }
};

TEST(MediaPolicyTest, ARealmThatSaysNothingPassesTheProfileThroughByDefault) {
  DefaultFixture f("tcp");

  f.receive(f.caller, PolicyFixture::with_body(f.invite()));

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::Mirror);
}

// A server default of reading the leg from its transport applies instead.
TEST(MediaPolicyTest, ARealmThatSaysNothingFollowsAConfiguredServerDefault) {
  DefaultFixture f("wss");
  f.config->behaviour.profiles = MediaPolicy::Profiles::FromTransport;

  f.receive(f.caller, PolicyFixture::with_body(f.invite()));

  const auto seen = f.engine->seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen[0].target, Flags::Profile::WebRtc);
}

// An unreadable profile name leaves the realm's policy as it was.
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

// RFC 7118 requires no WebRTC, so a realm can say its WebSocket clients are phones and are not offered ICE
// and DTLS.
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

// Nothing about a flow says a phone wants SRTP, so this profile can only be asked for.
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

// Not anchoring is the same outcome as having no engine: the description travels as it arrived.
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

// A realm's policy covers a leg that has not described itself. It never contradicts one that has: an answer
// goes back in the profile of the offer it answers.
TEST(MediaPolicyTest, ARealmPolicyDoesNotContradictWhatALegHasSaid) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::WebRtc), "udp");

  f.receive(f.caller, f.invite_with_body());
  f.receive(f.callee, f.ok_with_body());

  const auto seen = f.engine->seen();
  ASSERT_GE(seen.size(), 2u);

  // Alice offered plain RTP, so her answer is produced as plain RTP.
  EXPECT_EQ(seen.back().target, Flags::Profile::PlainRtp);
}

// The policy is decided once and kept on the call: a re-INVITE travels in-dialog and never looks a realm up.
TEST(MediaPolicyTest, TheCallKeepsThePolicyForTheRequestsThatFollow) {
  PolicyFixture f(with_profiles(MediaPolicy::Profiles::WebRtc), "udp");

  f.receive(f.caller, f.invite_with_body());

  auto call = f.call();
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(call->media_policy.anchor);
  EXPECT_EQ(call->media_policy.profiles, MediaPolicy::Profiles::WebRtc);
}
