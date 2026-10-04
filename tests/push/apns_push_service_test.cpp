//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/apns_push_service.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../helpers/http2_server_helper.h"
#include "../helpers/push_keys_helper.h"
#include "../mocks/logger_mock.h"
#include "config.h"

using namespace athenasip;
using athenasip::push::ApnsPushService;
using athenasip::push::Notification;

namespace {

// RFC 8599 10's examples.
constexpr char kTeamId[] = "DEF123GHIJ";
constexpr char kVoipParam[] = "DEF123GHIJ.com.example.yourexampleapp.voip";
constexpr char kDeviceToken[] = "00fc13adff78512";
constexpr char kKeyId[] = "ABC123DEFG";

// 2026-10-04T12:00:00Z.
constexpr std::int64_t kStart = 1790164800;

Notification binding(const std::string& param = kVoipParam, const std::string& prid = kDeviceToken) {
  Notification notification;
  notification.provider = "apns";
  notification.param = param;
  notification.prid = prid;
  return notification;
}

Notification refresh(const std::string& param = kVoipParam) {
  auto notification = binding(param);
  notification.reason = Notification::Reason::Refresh;
  return notification;
}

// A stand-in for APNs: 200 with an apns-id, or whatever status and reason the test sets.
struct FakeApns {
  std::atomic<unsigned> status{200};
  std::string reason;
  std::mutex mutex;

  TestHttp2Server server{[this](const TestHttp2Server::Request&) {
    TestHttp2Server::Reply reply;
    reply.status = status;
    reply.headers = {{"apns-id", "EC1BF194-B3B2-4A3E-8B8A-2E5D7A5D0C1F"}};
    if (reply.status != 200) {
      std::lock_guard<std::mutex> lock(mutex);
      reply.headers.emplace_back("content-type", "application/json");
      reply.body = "{\"reason\":\"" + reason + "\",\"timestamp\":1790164800000}";
    }
    return reply;
  }};

  void answer(unsigned with, const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex);
    status = with;
    reason = why;
  }
};

struct Fixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  push_test::Key key = push_test::generate_ec();
  push_test::TempFile key_file{push_test::private_pem(key)};
  std::shared_ptr<ApnsPushService> service = std::make_shared<ApnsPushService>(logger, std::make_shared<types::URL>("apns://"));
  std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>>(kStart);

  Fixture() {
    service->set_clock([now = now]() { return std::chrono::system_clock::time_point(std::chrono::seconds(now->load())); });
  }

  void advance(std::chrono::seconds by) { *now += by.count(); }

  std::string yaml(const std::string& extra = "") const {
    return "key_file: " + key_file.path() + "\nkey_id: " + kKeyId + "\nteam_id: " + kTeamId + "\nca_file: " + TestHttp2Server::ca_file() + "\n" + extra;
  }

  bool configure_with(const std::string& text) { return service->configure(YAML::Load(text), Config(logger)); }

  bool configure(const TestHttp2Server& server, const std::string& extra = "") { return configure_with(yaml("api_base: " + server.url("") + "\n" + extra)); }

  // The token a request carried, checked against the key's public half.
  std::optional<push_test::Token> token(const TestHttp2Server::Request& request) const {
    const auto authorization = request.header("authorization");
    if (authorization.rfind("bearer ", 0) != 0) return std::nullopt;
    auto token = push_test::split(authorization.substr(7));
    if (!token || !push_test::verify_es256(*token, push_test::ec_public_key(push_test::uncompressed_point(key)))) return std::nullopt;
    return token;
  }
};

}  // namespace

TEST(ApnsPushServiceTest, IsNamedForItsPnProviderValue) {
  Fixture fixture;
  // RFC 8599 10: the 'pn-provider' value is "apns".
  EXPECT_EQ(fixture.service->name(), "apns");
  EXPECT_EQ(fixture.service->kind(), "push");
  EXPECT_TRUE(fixture.service->capabilities().empty());
}

// RFC 8599 10: pn-param is the Team ID and the topic, pn-prid the device token.
TEST(ApnsPushServiceTest, AcceptsTheRfc8599Example) {
  Fixture fixture;
  EXPECT_TRUE(fixture.service->accepts(binding()));

  ASSERT_TRUE(fixture.configure_with(fixture.yaml()));
  EXPECT_TRUE(fixture.service->accepts(binding()));
}

// RFC 8599 10: the first period separates the Team ID; the bundle ID may hold more.
TEST(ApnsPushServiceTest, SplitsTheTeamIdAtTheFirstPeriod) {
  Fixture fixture;
  ASSERT_TRUE(fixture.configure_with(fixture.yaml()));

  EXPECT_TRUE(fixture.service->accepts(binding("DEF123GHIJ.com.example.app")));
  EXPECT_TRUE(fixture.service->accepts(binding("DEF123GHIJ.app.voip")));
  EXPECT_TRUE(fixture.service->accepts(binding("DEF123GHIJ.uk.co.example.my-app.voip")));
}

TEST(ApnsPushServiceTest, RefusesAPnParamWithoutATeamIdAndATopic) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(binding("")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ")));
  EXPECT_FALSE(fixture.service->accepts(binding(".com.example.app.voip")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ.")));
}

TEST(ApnsPushServiceTest, RefusesATopicThatIsNotABundleId) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ.com..example")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ.com.example.app.")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ.com.example app")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF123GHIJ.com.example\r\nx-injected: 1")));
  EXPECT_FALSE(fixture.service->accepts(binding("DEF/23GHIJ.com.example.app")));
}

// The device token is hex, of whatever length Apple gives it.
TEST(ApnsPushServiceTest, AcceptsOnlyAHexDeviceToken) {
  Fixture fixture;
  EXPECT_TRUE(fixture.service->accepts(binding(kVoipParam, "740F4707BEBCF74F9B7C25D48E3358945F6AA01DA5DDB387462C7EAF61BB78AD")));
  EXPECT_FALSE(fixture.service->accepts(binding(kVoipParam, "")));
  EXPECT_FALSE(fixture.service->accepts(binding(kVoipParam, "00fc13adff7851g")));
  EXPECT_FALSE(fixture.service->accepts(binding(kVoipParam, "00fc13ad/../x")));
}

// The node signs as one team, and APNs would refuse another team's topic.
TEST(ApnsPushServiceTest, RefusesAnotherTeamsBinding) {
  Fixture fixture;
  ASSERT_TRUE(fixture.configure_with(fixture.yaml()));
  EXPECT_FALSE(fixture.service->accepts(binding("XYZ987WVUT.com.example.yourexampleapp.voip")));
}

TEST(ApnsPushServiceTest, ConfiguresFromAKeyItsIdAndTheTeam) {
  Fixture fixture;
  EXPECT_TRUE(fixture.configure_with(fixture.yaml()));
  EXPECT_TRUE(fixture.service->health());

  EXPECT_TRUE(fixture.configure_with(fixture.yaml("environment: sandbox\nttl: 0\n")));
  EXPECT_TRUE(fixture.configure_with(fixture.yaml("environment: production\n")));
}

TEST(ApnsPushServiceTest, RefusesConfigurationWithoutWhatItNeeds) {
  Fixture fixture;
  const auto key = "key_file: " + fixture.key_file.path() + "\n";
  const auto key_id = std::string("key_id: ") + kKeyId + "\n";
  const auto team_id = std::string("team_id: ") + kTeamId + "\n";

  EXPECT_FALSE(fixture.service->configure(YAML::Node(), Config(fixture.logger)));
  EXPECT_FALSE(fixture.configure_with("ttl: 60"));
  EXPECT_FALSE(fixture.configure_with(key_id + team_id));
  EXPECT_FALSE(fixture.configure_with(key + team_id));
  EXPECT_FALSE(fixture.configure_with(key + key_id));
  EXPECT_FALSE(fixture.configure_with("key_file: /nonexistent/AuthKey.p8\n" + key_id + team_id));
  EXPECT_FALSE(fixture.service->health());
  EXPECT_FALSE(fixture.logger->lines(loggers::LogLevel::ERROR).empty());
}

// Apple issues P-256 keys and APNs takes ES256 only.
TEST(ApnsPushServiceTest, RefusesAKeyThatIsNotP256) {
  Fixture fixture;
  const auto tail = std::string("\nkey_id: ") + kKeyId + "\nteam_id: " + kTeamId + "\n";

  push_test::TempFile rsa(push_test::private_pem(push_test::generate_rsa()));
  push_test::TempFile p384(push_test::private_pem(push_test::generate_ec("P-384")));
  push_test::TempFile garbage("-----BEGIN PRIVATE KEY-----\nnope\n-----END PRIVATE KEY-----\n");

  EXPECT_FALSE(fixture.configure_with("key_file: " + rsa.path() + tail));
  EXPECT_FALSE(fixture.configure_with("key_file: " + p384.path() + tail));
  EXPECT_FALSE(fixture.configure_with("key_file: " + garbage.path() + tail));
  EXPECT_FALSE(fixture.service->health());
}

TEST(ApnsPushServiceTest, RefusesSettingsItCannotUse) {
  Fixture fixture;
  EXPECT_FALSE(fixture.configure_with(fixture.yaml("environment: staging\n")));
  EXPECT_FALSE(fixture.configure_with(fixture.yaml("api_base: http://api.push.apple.com\n")));
  EXPECT_FALSE(fixture.configure_with(fixture.yaml("ttl: -1\n")));
  EXPECT_FALSE(fixture.configure_with(fixture.yaml("ttl: soon\n")));
  EXPECT_FALSE(
      fixture.configure_with("key_file: " + fixture.key_file.path() + "\nkey_id: " + kKeyId + "\nteam_id: " + kTeamId + "\nca_file: /nonexistent/ca.pem\n"));
  EXPECT_FALSE(fixture.configure_with("key_file: " + fixture.key_file.path() + "\nkey_id: \"ABC 123\"\nteam_id: " + kTeamId + "\n"));
  EXPECT_FALSE(fixture.configure_with("key_file: " + fixture.key_file.path() + "\nkey_id: " + kKeyId + "\nteam_id: [DEF123GHIJ]\n"));
  EXPECT_FALSE(fixture.service->health());
}

TEST(ApnsPushServiceTest, CannotSendBeforeItIsConfigured) {
  Fixture fixture;
  EXPECT_FALSE(push_test::send(*fixture.service, binding()).ok);
}

TEST(ApnsPushServiceTest, SendsAVoipPushForACall) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  const auto status = push_test::send(*fixture.service, binding());
  ASSERT_TRUE(status.ok) << status.error;

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 1u);
  const auto& request = requests[0];

  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.path, std::string("/3/device/") + kDeviceToken);
  EXPECT_EQ(request.header("apns-topic"), "com.example.yourexampleapp.voip");
  EXPECT_EQ(request.header("apns-push-type"), "voip");
  EXPECT_EQ(request.header("apns-priority"), "10");
  EXPECT_EQ(request.header("apns-expiration"), std::to_string(kStart + 60));

  auto body = boost::json::parse(request.body).as_object();
  EXPECT_EQ(body, boost::json::parse("{\"reason\":\"call\"}").as_object());

  // Token-based provider trust: ES256 under the key's ID, issued by the team, now.
  const auto token = fixture.token(request);
  ASSERT_TRUE(token) << request.header("authorization");
  EXPECT_EQ(token->header.at("alg").as_string(), "ES256");
  EXPECT_EQ(token->header.at("kid").as_string(), kKeyId);
  EXPECT_EQ(token->claims.at("iss").as_string(), kTeamId);
  EXPECT_EQ(token->claims.at("iat").to_number<std::int64_t>(), kStart);
}

TEST(ApnsPushServiceTest, SaysWhenAPushIsARefresh) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  ASSERT_TRUE(push_test::send(*fixture.service, refresh()).ok);

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(boost::json::parse(requests[0].body), boost::json::parse("{\"reason\":\"refresh\"}"));
}

// A topic without the .voip service is the app's own, and gets an alert-type push.
TEST(ApnsPushServiceTest, SendsAnAlertPushToATopicThatIsNotVoip) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  ASSERT_TRUE(push_test::send(*fixture.service, binding("DEF123GHIJ.com.example.app")).ok);

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header("apns-topic"), "com.example.app");
  // Apple: a push carrying only content-available is a background push, sent at priority 5.
  EXPECT_EQ(requests[0].header("apns-push-type"), "background");
  EXPECT_EQ(requests[0].header("apns-priority"), "5");
  EXPECT_EQ(boost::json::parse(requests[0].body), boost::json::parse("{\"aps\":{\"content-available\":1},\"reason\":\"call\"}"));
}

TEST(ApnsPushServiceTest, ExpiresThePushAfterTheConfiguredTtl) {
  FakeApns apns;
  Fixture fixture;

  ASSERT_TRUE(fixture.configure(apns.server, "ttl: 300\n"));
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  // Apple's 0 is "deliver now or not at all", not a time in 1970.
  ASSERT_TRUE(fixture.configure(apns.server, "ttl: 0\n"));
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0].header("apns-expiration"), std::to_string(kStart + 300));
  EXPECT_EQ(requests[1].header("apns-expiration"), "0");
}

// Apple refuses a token over an hour old, and one replaced more often than every twenty
// minutes.
TEST(ApnsPushServiceTest, ReusesTheProviderTokenForBetweenTwentyAndFiftyMinutes) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  fixture.advance(std::chrono::minutes(19));
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  fixture.advance(std::chrono::minutes(1));
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  fixture.advance(std::chrono::minutes(30));
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 4u);
  for (const auto& request : requests) ASSERT_TRUE(fixture.token(request)) << request.header("authorization");

  EXPECT_EQ(requests[1].header("authorization"), requests[0].header("authorization"));
  EXPECT_EQ(requests[2].header("authorization"), requests[0].header("authorization"));

  EXPECT_NE(requests[3].header("authorization"), requests[0].header("authorization"));
  EXPECT_EQ(fixture.token(requests[3])->claims.at("iat").to_number<std::int64_t>(), kStart + 50 * 60);
}

TEST(ApnsPushServiceTest, KeepsOneConnectionToApns) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  for (int i = 0; i < 3; ++i) ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  EXPECT_EQ(apns.server.requests().size(), 3u);
  EXPECT_EQ(apns.server.connections(), 1);
}

TEST(ApnsPushServiceTest, FailsWithApplesReasonForAnythingBut200) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  apns.answer(410, "Unregistered");
  auto status = push_test::send(*fixture.service, binding());
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.error.find("410"), std::string::npos) << status.error;
  EXPECT_NE(status.error.find("Unregistered"), std::string::npos) << status.error;

  apns.answer(400, "BadDeviceToken");
  status = push_test::send(*fixture.service, binding());
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.error.find("400"), std::string::npos) << status.error;
  EXPECT_NE(status.error.find("BadDeviceToken"), std::string::npos) << status.error;
}

// ExpiredProviderToken asks for a new token, but never sooner than twenty minutes after
// the last.
TEST(ApnsPushServiceTest, ReplacesATokenApnsCallsExpiredOnlyOnceItMayBeReplaced) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  apns.answer(403, "ExpiredProviderToken");
  fixture.advance(std::chrono::minutes(5));
  ASSERT_FALSE(push_test::send(*fixture.service, binding()).ok);

  fixture.advance(std::chrono::minutes(1));
  ASSERT_FALSE(push_test::send(*fixture.service, binding()).ok);

  fixture.advance(std::chrono::minutes(20));
  ASSERT_FALSE(push_test::send(*fixture.service, binding()).ok);

  apns.answer(200, "");
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  const auto requests = apns.server.requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_EQ(requests[1].header("authorization"), requests[0].header("authorization"));
  EXPECT_EQ(requests[2].header("authorization"), requests[0].header("authorization"));
  EXPECT_NE(requests[3].header("authorization"), requests[0].header("authorization"));
}

TEST(ApnsPushServiceTest, SendsNothingForABindingItDoesNotAccept) {
  FakeApns apns;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(apns.server));

  EXPECT_FALSE(push_test::send(*fixture.service, binding("XYZ987WVUT.com.example.app.voip")).ok);
  EXPECT_FALSE(push_test::send(*fixture.service, binding(kVoipParam, "not-hex")).ok);
  EXPECT_TRUE(apns.server.requests().empty());
}

// iOS 13 and later stop delivering VoIP pushes to an app that takes one without reporting a call, so a binding on
// a VoIP topic is never pushed to refresh (RFC 8599 5.5); one on any other topic is.
TEST(ApnsPushServiceTest, AVoipTopicIsNeverPushedToRefresh) {
  Fixture fixture;

  EXPECT_FALSE(fixture.service->refreshes(binding(kVoipParam)));
  EXPECT_TRUE(fixture.service->refreshes(binding("DEF123GHIJ.com.example.app")));
}
