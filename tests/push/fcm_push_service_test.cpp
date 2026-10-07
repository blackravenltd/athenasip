//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/fcm_push_service.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/https_server_helper.h"
#include "../helpers/push_keys_helper.h"
#include "../mocks/logger_mock.h"
#include "config.h"

using namespace athenasip;
using athenasip::push::FcmPushService;
using athenasip::push::Notification;

namespace {

constexpr char kMessagesPath[] = "/v1/projects/athena-test-1/messages:send";

Notification binding(const std::string& project = "athena-test-1", const std::string& token = "fcm-registration-token:APA91bH") {
  Notification notification;
  notification.provider = "fcm";
  notification.param = project;
  notification.prid = token;
  return notification;
}

// Decodes application/x-www-form-urlencoded.
std::map<std::string, std::string> parse_form(const std::string& body) {
  auto decode = [](const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '+') {
        out += ' ';
      } else if (value[i] == '%' && i + 2 < value.size()) {
        out += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        out += value[i];
      }
    }
    return out;
  };

  std::map<std::string, std::string> fields;
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto end = std::min(body.find('&', start), body.size());
    const auto part = body.substr(start, end - start);
    const auto eq = part.find('=');
    if (eq != std::string::npos) fields[decode(part.substr(0, eq))] = decode(part.substr(eq + 1));
    start = end + 1;
  }
  return fields;
}

// A stand-in for both of Google's endpoints: the token endpoint and the FCM v1 API.
struct FakeGoogle {
  std::atomic<int> tokens_issued{0};
  std::atomic<unsigned> token_status{200};
  std::atomic<long> expires_in{3599};
  std::atomic<unsigned> send_status{200};

  TestHttpsServer server{[this](const TestHttpsServer::Request& request) {
    TestHttpsServer::Reply reply;
    reply.headers = {{"Content-Type", "application/json; charset=UTF-8"}};

    if (request.target == "/token") {
      reply.status = token_status;
      if (reply.status == 200) {
        const int issued = ++tokens_issued;
        reply.body = "{\"access_token\":\"ya29.token-" + std::to_string(issued) + "\",\"expires_in\":" + std::to_string(expires_in.load()) +
                     ",\"token_type\":\"Bearer\"}";
      } else {
        reply.body = "{\"error\":\"invalid_grant\",\"error_description\":\"Invalid JWT Signature.\"}";
      }
    } else {
      reply.status = send_status;
      reply.body = reply.status == 200 ? "{\"name\":\"projects/athena-test-1/messages/0:1\"}" : "{\"error\":{\"code\":" + std::to_string(reply.status) + "}}";
    }
    return reply;
  }};

  std::vector<TestHttpsServer::Request> sent() const {
    std::vector<TestHttpsServer::Request> found;
    for (auto& request : server.requests()) {
      if (request.target != "/token") found.push_back(request);
    }
    return found;
  }
};

struct Fixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  push_test::Key key = push_test::generate_rsa();
  std::shared_ptr<FcmPushService> service = std::make_shared<FcmPushService>(logger, std::make_shared<types::URL>("fcm://"));
  std::unique_ptr<push_test::TempFile> account;

  std::string account_json(const std::string& token_uri, const std::string& pem) const {
    boost::json::object json;
    json["type"] = "service_account";
    json["project_id"] = "athena-test-1";
    json["private_key_id"] = "0123456789abcdef";
    json["private_key"] = pem;
    json["client_email"] = "athenasip@athena-test-1.iam.gserviceaccount.com";
    json["token_uri"] = token_uri;
    return boost::json::serialize(json);
  }

  bool configure_with(const std::string& json, const std::string& extra = "") {
    account = std::make_unique<push_test::TempFile>(json);
    const std::string yaml = "service_account: " + account->path() + "\nca_file: " + TestHttpsServer::ca_file() + "\n" + extra;
    return service->configure(YAML::Load(yaml), Config(logger));
  }

  bool configure(const TestHttpsServer& server, const std::string& extra = "") {
    return configure_with(account_json(server.url("/token"), push_test::private_pem(key)), "api_base: " + server.url("") + "\n" + extra);
  }
};

}  // namespace

TEST(FcmPushServiceTest, IsNamedForItsPnProviderValue) {
  Fixture fixture;
  // RFC 8599 11: the 'pn-provider' value is "fcm".
  EXPECT_EQ(fixture.service->name(), "fcm");
  EXPECT_EQ(fixture.service->kind(), "push");
  EXPECT_TRUE(fixture.service->capabilities().empty());
}

// RFC 8599 11: pn-param is the project ID and pn-prid the registration token; both are needed.
TEST(FcmPushServiceTest, AcceptsAProjectIdAndARegistrationToken) {
  Fixture fixture;
  EXPECT_TRUE(fixture.service->accepts(binding()));
  EXPECT_TRUE(fixture.service->accepts(binding("example.com:legacy-project")));
}

TEST(FcmPushServiceTest, RefusesABindingWithoutPnParam) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(binding("")));
}

TEST(FcmPushServiceTest, RefusesABindingWithoutPnPrid) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(binding("athena-test-1", "")));
}

TEST(FcmPushServiceTest, RefusesAProjectIdThatWouldLeaveThePath) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(binding("../other")));
  EXPECT_FALSE(fixture.service->accepts(binding("project/messages:send?x")));
  EXPECT_FALSE(fixture.service->accepts(binding("pro ject")));
}

TEST(FcmPushServiceTest, ConfiguresFromAServiceAccount) {
  Fixture fixture;
  EXPECT_TRUE(fixture.configure_with(fixture.account_json("https://oauth2.googleapis.com/token", push_test::private_pem(fixture.key))));
  EXPECT_TRUE(fixture.service->health());
}

TEST(FcmPushServiceTest, RefusesConfigurationWithoutAServiceAccount) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->configure(YAML::Node(), Config(fixture.logger)));
  EXPECT_FALSE(fixture.service->configure(YAML::Load("ttl: 60"), Config(fixture.logger)));
  EXPECT_FALSE(fixture.service->configure(YAML::Load("service_account: /nonexistent/firebase.json"), Config(fixture.logger)));
  EXPECT_FALSE(fixture.service->health());
  EXPECT_FALSE(fixture.logger->lines(loggers::LogLevel::ERROR).empty());
}

TEST(FcmPushServiceTest, RefusesAServiceAccountThatIsNotUsable) {
  Fixture fixture;
  const auto pem = push_test::private_pem(fixture.key);

  EXPECT_FALSE(fixture.configure_with("not json"));
  EXPECT_FALSE(fixture.configure_with("[]"));

  auto without = [&](const char* field) {
    auto json = boost::json::parse(fixture.account_json("https://oauth2.googleapis.com/token", pem)).as_object();
    json.erase(field);
    return boost::json::serialize(json);
  };
  EXPECT_FALSE(fixture.configure_with(without("client_email")));
  EXPECT_FALSE(fixture.configure_with(without("private_key")));
  EXPECT_FALSE(fixture.configure_with(without("token_uri")));

  EXPECT_FALSE(fixture.configure_with(fixture.account_json("http://oauth2.googleapis.com/token", pem)));
  EXPECT_FALSE(fixture.configure_with(fixture.account_json("https://oauth2.googleapis.com/token", "-----BEGIN PRIVATE KEY-----\nnope\n")));

  // RS256 needs an RSA key.
  EXPECT_FALSE(fixture.configure_with(fixture.account_json("https://oauth2.googleapis.com/token", push_test::private_pem(push_test::generate_ec()))));
}

TEST(FcmPushServiceTest, RefusesUnusableOptions) {
  Fixture fixture;
  const auto json = fixture.account_json("https://oauth2.googleapis.com/token", push_test::private_pem(fixture.key));

  EXPECT_FALSE(fixture.configure_with(json, "api_base: http://fcm.googleapis.com"));
  EXPECT_FALSE(fixture.configure_with(json, "ttl: -5"));

  push_test::TempFile account(json);
  EXPECT_FALSE(fixture.service->configure(YAML::Load("service_account: " + account.path() + "\nca_file: /nonexistent/ca.pem"), Config(fixture.logger)));
}

// Google's OAuth 2.0 for service accounts: the JWT bearer grant of RFC 7523, an RS256
// assertion naming the account, the FCM scope and the token endpoint as audience.
TEST(FcmPushServiceTest, ObtainsAnAccessTokenWithASignedAssertion) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  const auto before = push_test::now_seconds();
  const auto status = push_test::send(*fixture.service, binding());
  ASSERT_TRUE(status.ok) << status.error;

  const auto requests = google.server.requests();
  ASSERT_GE(requests.size(), 1u);
  const auto& request = requests[0];

  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.target, "/token");
  EXPECT_EQ(request.header("Content-Type"), "application/x-www-form-urlencoded");

  // RFC 7523 2.1: grant_type and assertion.
  const auto form = parse_form(request.body);
  EXPECT_EQ(form.at("grant_type"), "urn:ietf:params:oauth:grant-type:jwt-bearer");
  ASSERT_EQ(form.count("assertion"), 1u);

  const auto assertion = push_test::split(form.at("assertion"));
  ASSERT_TRUE(assertion);
  EXPECT_EQ(assertion->header.at("alg").as_string(), "RS256");
  EXPECT_EQ(assertion->header.at("typ").as_string(), "JWT");
  EXPECT_EQ(assertion->header.at("kid").as_string(), "0123456789abcdef");
  EXPECT_TRUE(push_test::verify_rs256(*assertion, fixture.key));

  EXPECT_EQ(assertion->claims.at("iss").as_string(), "athenasip@athena-test-1.iam.gserviceaccount.com");
  EXPECT_EQ(assertion->claims.at("scope").as_string(), "https://www.googleapis.com/auth/firebase.messaging");
  EXPECT_EQ(assertion->claims.at("aud").as_string(), google.server.url("/token"));

  const auto iat = assertion->claims.at("iat").to_number<std::int64_t>();
  const auto exp = assertion->claims.at("exp").to_number<std::int64_t>();
  EXPECT_GE(iat, before - 1);
  EXPECT_LE(iat, push_test::now_seconds() + 1);
  EXPECT_GT(exp, iat);
  EXPECT_LE(exp - iat, 3600);
}

TEST(FcmPushServiceTest, SendsAHighPriorityDataMessageToTheProject) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  const auto sent = google.sent();
  ASSERT_EQ(sent.size(), 1u);
  const auto& request = sent[0];

  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.target, kMessagesPath);
  EXPECT_EQ(request.header("Authorization"), "Bearer ya29.token-1");
  EXPECT_EQ(request.header("Content-Type"), "application/json; charset=UTF-8");

  const auto expected =
      boost::json::parse(R"({"message":{"token":"fcm-registration-token:APA91bH","android":{"priority":"high","ttl":"60s"},"data":{"reason":"call"}}})");
  EXPECT_EQ(boost::json::parse(request.body), expected) << request.body;
}

TEST(FcmPushServiceTest, SaysWhyTheClientIsBeingWoken) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server, "ttl: 30"));

  auto refresh = binding();
  refresh.reason = Notification::Reason::Refresh;
  ASSERT_TRUE(push_test::send(*fixture.service, refresh).ok);

  const auto sent = google.sent();
  ASSERT_EQ(sent.size(), 1u);
  const auto body = boost::json::parse(sent[0].body);
  EXPECT_EQ(body.at("message").at("data").at("reason").as_string(), "refresh");
  EXPECT_EQ(body.at("message").at("android").at("ttl").as_string(), "30s");
}

TEST(FcmPushServiceTest, ReusesTheAccessTokenUntilItNearlyExpires) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  ASSERT_TRUE(push_test::send(*fixture.service, binding("athena-test-1", "another-device")).ok);

  EXPECT_EQ(google.tokens_issued.load(), 1);
  const auto sent = google.sent();
  ASSERT_EQ(sent.size(), 3u);
  for (const auto& request : sent) EXPECT_EQ(request.header("Authorization"), "Bearer ya29.token-1");
}

TEST(FcmPushServiceTest, PushesThatArriveTogetherShareOneTokenRequest) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  std::atomic<int> succeeded{0};
  std::atomic<int> answered{0};
  for (int i = 0; i < 4; ++i) {
    fixture.service->send(detail::get_global_io_context().get_executor(), binding(), [&](plugins::Status status) {
      if (status.ok) ++succeeded;
      ++answered;
    });
  }

  for (int i = 0; i < 1500 && answered.load() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(succeeded.load(), 4);
  EXPECT_EQ(google.tokens_issued.load(), 1);
}

TEST(FcmPushServiceTest, AsksForANewTokenWhenTheOldOneIsAboutToLapse) {
  FakeGoogle google;
  google.expires_in = 30;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  EXPECT_EQ(google.tokens_issued.load(), 2);
  const auto sent = google.sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[1].header("Authorization"), "Bearer ya29.token-2");
}

TEST(FcmPushServiceTest, ATokenRefusedByFcmIsNotUsedAgain) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  google.send_status = 401;
  EXPECT_FALSE(push_test::send(*fixture.service, binding()).ok);

  google.send_status = 200;
  ASSERT_TRUE(push_test::send(*fixture.service, binding()).ok);

  EXPECT_EQ(google.tokens_issued.load(), 2);
  EXPECT_EQ(google.sent().back().header("Authorization"), "Bearer ya29.token-2");
}

TEST(FcmPushServiceTest, FailsWithoutSendingWhenNoTokenIsIssued) {
  FakeGoogle google;
  google.token_status = 400;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  const auto status = push_test::send(*fixture.service, binding());
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.error.find("400"), std::string::npos) << status.error;
  EXPECT_TRUE(google.sent().empty());
}

TEST(FcmPushServiceTest, AnythingButOkIsAFailureCarryingTheStatus) {
  for (const unsigned code : {201u, 400u, 403u, 404u, 429u, 500u, 503u}) {
    FakeGoogle google;
    google.send_status = code;
    Fixture fixture;
    ASSERT_TRUE(fixture.configure(google.server));

    const auto status = push_test::send(*fixture.service, binding());
    EXPECT_FALSE(status.ok) << code;
    EXPECT_NE(status.error.find(std::to_string(code)), std::string::npos) << status.error;
  }
}

TEST(FcmPushServiceTest, RefusesToSendWhatItDoesNotAccept) {
  FakeGoogle google;
  Fixture fixture;
  ASSERT_TRUE(fixture.configure(google.server));

  EXPECT_FALSE(push_test::send(*fixture.service, binding("")).ok);
  EXPECT_TRUE(google.server.requests().empty());
}

TEST(FcmPushServiceTest, RefusesToSendUnconfigured) {
  Fixture fixture;
  EXPECT_FALSE(push_test::send(*fixture.service, binding()).ok);
}
