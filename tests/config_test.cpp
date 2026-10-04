//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio/ip/address.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "mocks/logger_mock.h"

using namespace athenasip;

namespace {

// A temporary configuration file per test: Config loads from a path, as the node does at startup.
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

// websocket.tls with a certificate and key configures a wss listener (RFC 7118).
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

// The WebSocket listener is plain ws:// unless websocket.tls is set.
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

// websocket.tls without both certificate and key is refused, never downgraded to ws://.
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

// A disabled listener needs no port.
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

// An enabled listener without a port is refused.
TEST(ConfigTest, AListenerThatIsOnStillNeedsAPort) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "udp:\n  enable: true\n");

  bool ok = false;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// A configuration that sets only the node id starts, on drivers that need no external service.
TEST(ConfigTest, DefaultsNeedNoExternalService) {
  ConfigFile file("sip:\n  node_id: test-node\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->db_url, "memory://");
  EXPECT_EQ(config->media_url, "builtin://");
}

// Timer C must be larger than 3 minutes (RFC 3261 16.6 step 11); a lower value is ignored and the
// default kept.
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

// Min-SE must not be lower than 90 seconds (RFC 4028 section 8.1); a lower value is not taken.
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

// An unknown media_profile is refused.
TEST(ConfigTest, AnUnknownMediaProfileIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "behaviour:\n  media_profile: web-rtc\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// The effective configuration (--print-config) shows the behaviour defaults.
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

// OPTIONS qualifying of registered clients is off unless qualify_interval is set.
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

// A qualify_interval that is too short, too long, negative or not a number is refused.
TEST(ConfigTest, AnUnusableQualifyIntervalIsRefused) {
  for (const auto* value : {"2", "-1", "often", "100000"}) {
    ConfigFile file(std::string("sip:\n  node_id: test-node\nbehaviour:\n  qualify_interval: ") + value + "\n");
    bool ok = true;
    file.load(ok);
    EXPECT_FALSE(ok) << value;
  }
}

// rewrite_contact defaults to false, and anything but a boolean is refused.
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

// sip.localnet is a list of prefixes (a bare address is a host prefix); an invalid one is refused.
// public_port is per listener.
TEST(ConfigTest, LocalnetAndPublicPortsAreRead) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n  public_address: 203.0.113.5\n  localnet: [\"10.0.0.0/8\", \"192.168.1.7\", \"fd00::/8\"]\n"
      "udp:\n  port: 5060\n  public_port: 5080\n"
      "tls:\n  enable: false\n  port: 5061\n  public_port: 5081\n");

  bool ok = false;
  auto config = file.load(ok);
  ASSERT_TRUE(ok);

  EXPECT_EQ(config->udp_public_port, 5080);
  EXPECT_EQ(config->tls_public_port, 5081);
  EXPECT_TRUE(config->in_localnet(boost::asio::ip::make_address("10.1.2.3")));
  EXPECT_TRUE(config->in_localnet(boost::asio::ip::make_address("192.168.1.7")));
  EXPECT_FALSE(config->in_localnet(boost::asio::ip::make_address("192.168.1.8")));
  EXPECT_TRUE(config->in_localnet(boost::asio::ip::make_address("fd12::1")));
  EXPECT_FALSE(config->in_localnet(boost::asio::ip::make_address("203.0.113.9")));

  const auto effective = config->effective_yaml();
  EXPECT_NE(effective.find("localnet"), std::string::npos) << effective;
  EXPECT_NE(effective.find("public_port: 5080"), std::string::npos) << effective;

  ConfigFile wrong("sip:\n  node_id: test-node\n  localnet: [\"10.0.0.0/33\"]\n");
  ok = true;
  wrong.load(ok);
  EXPECT_FALSE(ok);
}

// The cluster listener is off by default, and enabling it without ca, cert and key is refused.
TEST(ConfigTest, TheClusterListenerNeedsItsCertificates) {
  ConfigFile off("sip:\n  node_id: test-node\n");
  bool ok = false;
  auto config = off.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(config->cluster_enable);
  EXPECT_EQ(config->cluster_port, 5062);

  ConfigFile on(
      "sip:\n  node_id: test-node\n"
      "cluster:\n  enable: true\n  port: 5070\n  ca: /ca/ca.crt\n  cert: /ca/node-a.crt\n  key: /ca/node-a.key\n");
  config = on.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->cluster_enable);
  EXPECT_EQ(config->cluster_port, 5070);
  EXPECT_EQ(config->cluster_ca, "/ca/ca.crt");

  ConfigFile bare("sip:\n  node_id: test-node\ncluster:\n  enable: true\n");
  ok = true;
  bare.load(ok);
  EXPECT_FALSE(ok);
}

// The address peers dial is cluster.advertise, else the bound cluster.address, else (wildcard bind)
// sip.public_address. The node certificate must name it.
TEST(ConfigTest, TheClusterAddressAPeerDialsIsSaidOrWorkedOut) {
  const std::string certificates = "  ca: /ca/ca.crt\n  cert: /ca/node-a.crt\n  key: /ca/node-a.key\n";
  bool ok = false;

  ConfigFile off("sip:\n  node_id: test-node\n");
  EXPECT_FALSE(off.load(ok)->advertised_cluster().has_value());

  ConfigFile said("sip:\n  node_id: test-node\n  public_address: 203.0.113.5\ncluster:\n  enable: true\n  advertise: node-a.internal\n" + certificates);
  auto found = said.load(ok)->advertised_cluster();
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, "node-a.internal");
  EXPECT_EQ(found->port, 5062);

  ConfigFile bound("sip:\n  node_id: test-node\n  public_address: 203.0.113.5\ncluster:\n  enable: true\n  address: 10.0.0.1\n  port: 5070\n" + certificates);
  found = bound.load(ok)->advertised_cluster();
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, "10.0.0.1");
  EXPECT_EQ(found->port, 5070);

  ConfigFile wildcard("sip:\n  node_id: test-node\n  public_address: 203.0.113.5\ncluster:\n  enable: true\n" + certificates);
  found = wildcard.load(ok)->advertised_cluster();
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, "203.0.113.5");
}

// Each API rate limit is set separately; unset values keep their defaults, and zero turns a limit off.
TEST(ConfigTest, TheRateLimitsAreConfigurable) {
  bool ok = false;

  ConfigFile defaults("sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  api:\n    enable: true\n");
  auto config = defaults.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_api_limit_open.burst, 30u);
  EXPECT_EQ(config->http_api_limit_open.per_minute, 30u);
  EXPECT_EQ(config->http_api_limit_login_source.burst, 10u);
  EXPECT_EQ(config->http_api_limit_login_user.per_minute, 1u);
  EXPECT_EQ(config->http_api_limit_session.burst, 60u);
  EXPECT_EQ(config->http_api_limit_session.per_minute, 300u);

  ConfigFile set(
      "sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  api:\n    enable: true\n"
      "    rate_limits:\n      session:\n        burst: 200\n        per_minute: 1200\n      login_user:\n        per_minute: 0\n");
  config = set.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_api_limit_session.burst, 200u);
  EXPECT_EQ(config->http_api_limit_session.per_minute, 1200u);
  EXPECT_EQ(config->http_api_limit_login_user.burst, 5u);
  EXPECT_EQ(config->http_api_limit_login_user.per_minute, 0u);
  EXPECT_EQ(config->http_api_limit_open.burst, 30u);

  ConfigFile wrong("sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  api:\n    enable: true\n    rate_limits:\n      open:\n        burst: lots\n");
  ok = true;
  wrong.load(ok);
  EXPECT_FALSE(ok);
}

// Admin HTTPS is off by default, listens beside plain HTTP on its own port, and uses the tls
// section's certificate unless http.tls names another.
TEST(ConfigTest, TheAdminListenerOffersHttpsWhenAsked) {
  bool ok = false;

  ConfigFile off("sip:\n  node_id: test-node\nhttp:\n  port: 8080\n");
  auto config = off.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(config->http_tls_enable);

  ConfigFile shared(
      "sip:\n  node_id: test-node\ntls:\n  enable: false\n  cert_pem_filename: /tls/node.cer\n  key_pem_filename: /tls/node.key\n"
      "http:\n  address: 10.0.0.1\n  port: 8080\n  tls:\n    enable: true\n");
  config = shared.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->http_tls_enable);
  EXPECT_EQ(config->http_tls_port, 8443);
  EXPECT_EQ(config->http_tls_address, "10.0.0.1");
  EXPECT_EQ(config->http_tls_cert(), "/tls/node.cer");
  EXPECT_EQ(config->http_tls_key(), "/tls/node.key");
  EXPECT_EQ(config->http_port, 8080);

  ConfigFile own(
      "sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  tls:\n    enable: true\n    port: 9443\n"
      "    cert_pem_filename: /admin/admin.cer\n    key_pem_filename: /admin/admin.key\n");
  config = own.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->http_tls_port, 9443);
  EXPECT_EQ(config->http_tls_cert(), "/admin/admin.cer");

  // Enabled with no certificate available, it is refused.
  ConfigFile bare("sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  tls:\n    enable: true\n");
  ok = true;
  bare.load(ok);
  EXPECT_FALSE(ok);
}

// websocket.secure_port adds a wss listener beside the plain one; both are advertised, and the two
// ports must differ.
TEST(ConfigTest, ASecureWebSocketListenerSitsBesideThePlainOne) {
  bool ok = false;

  ConfigFile both(
      "sip:\n  node_id: test-node\n  public_address: 203.0.113.5\n"
      "tls:\n  enable: false\n  cert_pem_filename: /tls/node.cer\n  key_pem_filename: /tls/node.key\n"
      "websocket:\n  enable: true\n  port: 8088\n  secure_port: 8089\n");
  auto config = both.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->websocket_secure_port, 8089);
  EXPECT_EQ(config->websocket_cert(), "/tls/node.cer");

  bool ws = false;
  bool wss = false;
  for (const auto& transport : config->advertised_transports()) {
    if (transport.transport == "ws" && transport.port == 8088) ws = true;
    if (transport.transport == "wss" && transport.port == 8089 && transport.secure) wss = true;
  }
  EXPECT_TRUE(ws);
  EXPECT_TRUE(wss);

  ConfigFile same("sip:\n  node_id: test-node\nwebsocket:\n  enable: true\n  port: 8088\n  secure_port: 8088\n");
  ok = true;
  same.load(ok);
  EXPECT_FALSE(ok);
}

// log.level and log.format (text or json) default to debug and text; an unknown level is refused.
TEST(ConfigTest, TheLogSectionSetsTheLevelAndTheFormat) {
  bool ok = false;

  ConfigFile defaults("sip:\n  node_id: test-node\n");
  auto config = defaults.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->log_level, loggers::LogLevel::DEBUG);
  EXPECT_EQ(config->log_format, loggers::LogFormat::Text);

  ConfigFile set("sip:\n  node_id: test-node\nlog:\n  level: info\n  format: json\n");
  config = set.load(ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(config->log_level, loggers::LogLevel::INFO);
  EXPECT_EQ(config->log_format, loggers::LogFormat::Json);

  ConfigFile wrong("sip:\n  node_id: test-node\nlog:\n  level: chatty\n");
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

// Every session has an absolute expiry, which the datastore prunes on, so a lifetime of zero is refused.
TEST(ConfigTest, ASessionLifetimeOfZeroIsRefused) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "http:\n  port: 8080\n  api:\n    enable: true\n    session_lifetime: 0\n");

  bool ok = true;
  file.load(ok);

  EXPECT_FALSE(ok);
}

// session_idle: 0 turns idle expiry off.
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

  // What the file set.
  EXPECT_NE(effective.find("node_id: test-node"), std::string::npos);
  EXPECT_NE(effective.find("redis://127.0.0.1:6379"), std::string::npos);

  // Defaults the file did not set.
  EXPECT_NE(effective.find("session_min_se: 90"), std::string::npos);
  EXPECT_NE(effective.find("timer_t1_rtt_ms: 500"), std::string::npos);
  EXPECT_NE(effective.find("url: local://"), std::string::npos);
}

// http.api.tokens is not supported; a file that sets it is refused rather than silently ignored.
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

// A key the server parses (events.status_interval) appears once, even beside a plugin section that is
// carried through verbatim.
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

// config/config.example.yaml loads, and the defaults it documents match the code.
TEST(ConfigTest, TheShippedExampleConfigurationLoads) {
  const std::string path = std::string(ATHENA_TEST_SOURCE_DIR) + "/config/config.example.yaml";
  ASSERT_TRUE(std::filesystem::exists(path)) << path;

  auto logger = std::make_shared<MockLogger>();
  auto config = std::make_shared<Config>(logger);

  ASSERT_TRUE(config->load_from_yaml(path));

  // Spot-check the values the example documents as defaults.
  EXPECT_EQ(config->sip_media_timeout, 300u);
  EXPECT_EQ(config->sip_session_expires, 1800u);
  EXPECT_EQ(config->sip_session_min_se, 90u);
  EXPECT_EQ(config->sip_max_call_duration, 0u);
  EXPECT_FALSE(config->sip_require_session_timer);
  EXPECT_EQ(config->sip_timer_c_invite_proxy_ms, 240000u);
  EXPECT_EQ(config->sip_connect_timeout_ms, 4000u);
  EXPECT_EQ(config->http_api_session_lifetime, 12u * 60u * 60u);
  EXPECT_EQ(config->http_api_session_idle, 60u * 60u);

  // Monitors derive their staleness threshold from the status interval.
  EXPECT_EQ(config->events_status_interval, 30u);
  EXPECT_EQ(config->sip_flow_idle_timeout, 300u);
}
