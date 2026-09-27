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

TEST(ConfigTest, SessionLifetimesAreTakenFromTheApiSection) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_lifetime: 7200\n    session_idle: 900\n"
      "    tokens:\n      - token: t\n        scopes: [admin]\n");

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
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_lifetime: 0\n"
      "    tokens:\n      - token: t\n        scopes: [admin]\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// Zero is how an operator turns the idle rule off, which is a different thing from
// leaving it out.
TEST(ConfigTest, ASessionIdleOfZeroTurnsIdleExpiryOff) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_idle: 0\n"
      "    tokens:\n      - token: t\n        scopes: [admin]\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_api_session_idle, 0u);
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
}
