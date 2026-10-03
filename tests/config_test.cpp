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

// The config: localnet is a list of prefixes, a bare address is a prefix of one, and
// anything else stops the node. public_port belongs to each listener.
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

// The inter-node listener is off unless asked for, and asked for without certificates it
// would let anybody in, so that stops the node.
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

// What a node tells its peers to dial. The certificate a peer checks has to name it, so an
// operator can say; left unsaid it is the address the listener is bound to, and where that
// is every address, the one the node is told the world reaches it at.
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

// The rate limits are the operator's to set (Tom, 2026-10-03), each one separately, and
// what is not set keeps its default. Zero turns a limit off.
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

// HTTPS on the admin listener is off unless asked for, sits beside plain HTTP on a port of
// its own, and uses the tls section's certificate unless it names another.
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

  // Asked for with nothing to show is refused at start rather than found out by a browser.
  ConfigFile bare("sip:\n  node_id: test-node\nhttp:\n  port: 8080\n  tls:\n    enable: true\n");
  ok = true;
  bare.load(ok);
  EXPECT_FALSE(ok);
}

// A page served over HTTPS may only open a secure WebSocket, so a node that serves its
// console over HTTPS needs wss; a plain one beside it is still what a local page or a test
// client uses. Both, on a port each, and both advertised.
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
