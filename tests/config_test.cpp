//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "mocks/logger_mock.h"

using namespace athenasip;

namespace {

// A configuration file of its own per test, removed with the fixture. Config reads a
// path rather than a string, which is the same thing the node does at startup.
struct ConfigFile {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::filesystem::path path;

  explicit ConfigFile(const std::string& yaml) {
    path = std::filesystem::temp_directory_path() / ("athenasip-config-test-" + std::to_string(::getpid()) + "-" + std::to_string(_next()) + ".yaml");

    std::ofstream out(path);
    out << yaml;
  }

  ~ConfigFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::shared_ptr<Config> load(bool& ok) {
    auto config = std::make_shared<Config>(logger);
    ok = config->load_from_yaml(path.string());
    return config;
  }

 private:
  static int _next() {
    static int counter = 0;
    return ++counter;
  }
};

}  // namespace

// RFC 7118 over TLS. A browser will not open an insecure WebSocket from a page served
// over https, so this is what says a web client can reach the node at all.
TEST(ConfigTest, ReadsASecureWebsocketListener) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  address: 127.0.0.1\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  cert_pem_filename: \"../tls/snakeoil.cer\"\n"
      "  key_pem_filename: \"../tls/snakeoil.key\"\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->websocket_enable);
  EXPECT_TRUE(config->websocket_tls);
  EXPECT_EQ(config->websocket_port, 9501);
  EXPECT_EQ(config->websocket_cert_pem_filename, "../tls/snakeoil.cer");
  EXPECT_EQ(config->websocket_key_pem_filename, "../tls/snakeoil.key");
}

// ws:// is what the listener is without the section saying otherwise, and it stays for
// local development. Nothing about the plain listener changed when the secure one
// arrived.
TEST(ConfigTest, AWebsocketListenerIsPlainUnlessItSaysOtherwise) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  address: 0.0.0.0\n"
      "  port: 9500\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->websocket_enable);
  EXPECT_FALSE(config->websocket_tls);
}

// A listener asked to be secure with nothing to be secure with must not start. Falling
// back to ws:// would be a node quietly serving a browser in the clear, which is the
// failure nobody notices.
TEST(ConfigTest, ASecureWebsocketListenerWithoutCertificatesIsRefused) {
  ConfigFile without_key(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  cert_pem_filename: \"../tls/snakeoil.cer\"\n");

  bool ok = true;
  without_key.load(ok);
  EXPECT_FALSE(ok);

  ConfigFile without_cert(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  key_pem_filename: \"../tls/snakeoil.key\"\n");

  ok = true;
  without_cert.load(ok);
  EXPECT_FALSE(ok);

  ConfigFile with_neither(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n");

  ok = true;
  with_neither.load(ok);
  EXPECT_FALSE(ok);
}

// The node ships defaulting to what needs no external service, which is the whole of the
// "easy to install" principle in one line: a config that says nothing has to start.
// A listener turned off does not need a port. Refusing to start over the port of
// something that will never listen is the kind of thing that makes a server feel
// hostile to configure, and principle 3 is that it should not.
TEST(ConfigTest, AListenerThatIsOffNeedsNoPort) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "tls:\n  enable: false\n"
      "udp:\n  enable: true\n  port: 5060\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_FALSE(config->tls_enable);
  EXPECT_TRUE(config->udp_enable);
  EXPECT_EQ(config->udp_port, 5060);
}

// A listener that is on still does: binding somewhere nobody asked for is worse than
// saying the configuration is incomplete.
TEST(ConfigTest, AListenerThatIsOnStillNeedsAPort) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "udp:\n  enable: true\n");

  bool ok = false;
  file.load(ok);

  EXPECT_FALSE(ok);
}

TEST(ConfigTest, DefaultsNeedNoExternalService) {
  ConfigFile file("sip:\n  node_id: test-node\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->db_url, "memory://");
  EXPECT_EQ(config->media_url, "builtin://");
}

// RFC 3261 16.6 step 11: "The timer MUST be larger than 3 minutes." A node configured
// under that would give up on calls that are only still ringing, so the value is refused
// and the default kept rather than honoured.
TEST(ConfigTest, TimerCBelowTheRfcFloorIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "  timers:\n"
      "    c_invite_proxy_ms: 60000\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_GT(config->sip_timer_c_invite_proxy_ms, 180000u);
}

TEST(ConfigTest, TimerCAboveTheRfcFloorIsTaken) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "  timers:\n"
      "    c_invite_proxy_ms: 300000\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->sip_timer_c_invite_proxy_ms, 300000u);
}

// RFC 4028 section 8.1: the minimum a proxy quotes in a 422 "MUST NOT be lower than 90
// seconds". Section 4 explains why: it is a bit more than twice the longest a SIP
// transaction can take, so below it a refresh could not complete before the session it
// was refreshing expired.
TEST(ConfigTest, ASessionMinimumBelowTheRfcFloorIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "  session_min_se: 30\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_GE(config->sip_session_min_se, 90u);
}

TEST(ConfigTest, ASessionMinimumAboveTheRfcFloorIsTaken) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "  session_min_se: 600\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->sip_session_min_se, 600u);
}

// --- behaviour: the server's default for what realms leave unset ---

TEST(ConfigTest, WithNoBehaviourSectionTheShippedDefaultHolds) {
  ConfigFile file("sip:\n  node_id: test-node\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->behaviour.anchor);
  EXPECT_EQ(config->behaviour.profiles, types::MediaPolicy::Profiles::Mirror);
}

TEST(ConfigTest, TheBehaviourSectionSetsTheServerDefault) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "behaviour:\n  media_anchor: false\n  media_profile: transport\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_FALSE(config->behaviour.anchor);
  EXPECT_EQ(config->behaviour.profiles, types::MediaPolicy::Profiles::FromTransport);
}

// A misspelt profile is a node doing something other than what its operator wrote, so it
// does not start.
TEST(ConfigTest, AnUnknownMediaProfileIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "behaviour:\n  media_profile: web-rtc\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// It is what --print-config shows, because "what does this node do by default" is the
// question that file answers.
TEST(ConfigTest, TheEffectiveConfigurationShowsTheBehaviourDefault) {
  ConfigFile file("sip:\n  node_id: test-node\n");

  bool ok = false;
  auto config = file.load(ok);
  ASSERT_TRUE(ok);

  const auto effective = config->effective_yaml();
  EXPECT_NE(effective.find("behaviour:"), std::string::npos) << effective;
  EXPECT_NE(effective.find("media_profile: mirror"), std::string::npos) << effective;
  EXPECT_NE(effective.find("media_anchor: true"), std::string::npos) << effective;
}

// Probing registered clients with OPTIONS is something servers do when configured to and
// RFC 3261 does not ask for, so it is off unless the operator says how often.
TEST(ConfigTest, QualifyingIsOffUnlessAnIntervalIsGiven) {
  ConfigFile off("sip:\n  node_id: test-node\n");
  bool ok = false;
  auto config = off.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->behaviour_qualify_interval, 0u);
  EXPECT_NE(config->effective_yaml().find("qualify_interval: 0"), std::string::npos);

  ConfigFile on("sip:\n  node_id: test-node\nbehaviour:\n  qualify_interval: 60\n");
  config = on.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->behaviour_qualify_interval, 60u);
}

// Every few seconds is a flood across thousands of registrations, and a negative interval
// is a mistake. Both stop the node rather than being read as something else.
TEST(ConfigTest, AnUnusableQualifyIntervalIsRefused) {
  for (const auto* value : {"2", "-1", "often", "100000"}) {
    ConfigFile file(std::string("sip:\n  node_id: test-node\nbehaviour:\n  qualify_interval: ") + value + "\n");
    bool ok = true;
    file.load(ok);
    EXPECT_FALSE(ok) << value;
  }
}

// A proxy that rewrites a Contact changes what an endpoint said about itself, so it is off
// unless asked for, and anything but true or false stops the node.
TEST(ConfigTest, RewritingContactsIsOffUnlessAskedFor) {
  ConfigFile off("sip:\n  node_id: test-node\n");
  bool ok = false;
  auto config = off.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(config->behaviour_rewrite_contact);
  EXPECT_NE(config->effective_yaml().find("rewrite_contact: false"), std::string::npos);

  ConfigFile on("sip:\n  node_id: test-node\nbehaviour:\n  rewrite_contact: true\n");
  config = on.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->behaviour_rewrite_contact);

  ConfigFile wrong("sip:\n  node_id: test-node\nbehaviour:\n  rewrite_contact: sometimes\n");
  ok = true;
  wrong.load(ok);
  EXPECT_FALSE(ok);
}

TEST(ConfigTest, SessionLifetimesAreTakenFromTheApiSection) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_lifetime: 7200\n    session_idle: 900\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_api_session_lifetime, 7200u);
  EXPECT_EQ(config->http_api_session_idle, 900u);
}

// The absolute expiry is written on the session record and is what the datastore prunes
// on, so there is no such thing as a session without one: Redis has no non-positive
// SETEX to give it, and a record nothing expires is one that outlives the node.
TEST(ConfigTest, ASessionLifetimeOfZeroIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_lifetime: 0\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// Zero is how an operator turns the idle rule off, which is a different thing from
// leaving it out.
TEST(ConfigTest, ASessionIdleOfZeroTurnsIdleExpiryOff) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_idle: 0\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_api_session_idle, 0u);
}

// --- The effective configuration ---

TEST(ConfigTest, TheEffectiveConfigurationCarriesDefaultsNobodyWroteDown) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "datastore:\n  url: \"redis://127.0.0.1:6379\"\n");

  bool ok = false;
  auto config = file.load(ok);
  ASSERT_TRUE(ok);

  const auto effective = config->effective_yaml();

  // What was written.
  EXPECT_NE(effective.find("node_id: test-node"), std::string::npos);
  EXPECT_NE(effective.find("redis://127.0.0.1:6379"), std::string::npos);

  // And what was decided, which is the point: the interesting half of what a node runs
  // on is never in the file.
  EXPECT_NE(effective.find("session_min_se: 90"), std::string::npos);
  EXPECT_NE(effective.find("timer_t1_rtt_ms: 500"), std::string::npos);
  EXPECT_NE(effective.find("url: local://"), std::string::npos);
}

// Configured tokens were removed on 2026-10-01. A file that still sets them is refused,
// and says what to do instead, rather than being read as if they were not there - which
// would leave an operator unable to log in with no idea why.
TEST(ConfigTest, AConfigurationThatSetsApiTokensIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n"
      "    tokens:\n      - token: hunter2-do-not-print\n        scopes: [admin]\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

TEST(ConfigTest, TheEffectiveConfigurationCarriesPluginSectionsThrough) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "media:\n  url: \"rtpengine://127.0.0.1:22222\"\n  rtpengine:\n    timeout_ms: 750\n");

  bool ok = false;
  auto config = file.load(ok);
  ASSERT_TRUE(ok);

  const auto effective = config->effective_yaml();

  EXPECT_NE(effective.find("rtpengine:"), std::string::npos);
  EXPECT_NE(effective.find("timeout_ms: 750"), std::string::npos);
}

// Carrying a plugin section through means copying what the server did not parse, and
// events.status_interval is parsed. Emitting it from the field and again from the
// document put the key in twice and gave a reader two answers.
TEST(ConfigTest, TheEffectiveConfigurationSaysEachThingOnce) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "events:\n  url: \"mqtt://127.0.0.1:1883\"\n  status_interval: 45\n  mqtt:\n    keep_alive: 30\n");

  bool ok = false;
  auto config = file.load(ok);
  ASSERT_TRUE(ok);

  const auto effective = config->effective_yaml();

  std::size_t found = 0;
  for (std::size_t at = effective.find("status_interval"); at != std::string::npos; at = effective.find("status_interval", at + 1)) ++found;

  EXPECT_EQ(found, 1u);
  EXPECT_NE(effective.find("status_interval: 45"), std::string::npos);
  EXPECT_NE(effective.find("keep_alive: 30"), std::string::npos);
}

// The configuration this project ships as its example has to be one the node accepts.
// It is the first thing anybody copies, and every setting in it is written as its own
// default, so a key that has been renamed or removed shows up here rather than in
// somebody's log.
TEST(ConfigTest, TheShippedExampleConfigurationLoads) {
  const std::string path = std::string(ATHENA_TEST_SOURCE_DIR) + "/config/config.example.yaml";
  ASSERT_TRUE(std::filesystem::exists(path)) << path;

  auto logger = std::make_shared<MockLogger>();
  auto config = std::make_shared<Config>(logger);

  ASSERT_TRUE(config->load_from_yaml(path));

  // Spot-check the values the file claims are the defaults, because a comment that has
  // drifted from the code is worse than no comment.
  EXPECT_EQ(config->sip_media_timeout, 300u);
  EXPECT_EQ(config->sip_session_expires, 1800u);
  EXPECT_EQ(config->sip_session_min_se, 90u);
  EXPECT_EQ(config->sip_max_call_duration, 0u);
  EXPECT_FALSE(config->sip_require_session_timer);
  EXPECT_EQ(config->sip_timer_c_invite_proxy_ms, 240000u);
  EXPECT_EQ(config->sip_connect_timeout_ms, 4000u);
  EXPECT_EQ(config->http_api_session_lifetime, 12u * 60u * 60u);
  EXPECT_EQ(config->http_api_session_idle, 60u * 60u);

  // What a monitor's staleness threshold is a multiple of, so it is worth the example
  // saying it rather than leaving it to be discovered from the source.
  EXPECT_EQ(config->events_status_interval, 30u);
  EXPECT_EQ(config->sip_flow_idle_timeout, 300u);
}
