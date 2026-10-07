//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/webpush_push_service.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <map>
#include <memory>
#include <string>

#include "../helpers/https_server_helper.h"
#include "../helpers/push_keys_helper.h"
#include "../mocks/logger_mock.h"
#include "config.h"

using namespace athenasip;
using athenasip::push::Notification;
using athenasip::push::WebpushPushService;

namespace {

Notification subscription(const std::string& uri) {
  Notification notification;
  notification.provider = "webpush";
  notification.prid = uri;
  return notification;
}

struct Fixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  push_test::Key key = push_test::generate_ec();
  push_test::TempFile key_file{push_test::private_pem(key)};
  std::shared_ptr<WebpushPushService> service = std::make_shared<WebpushPushService>(logger, std::make_shared<types::URL>("webpush://"));

  bool configure(const std::string& extra = "", const std::string& subject = "mailto:ops@example.com") {
    const std::string yaml = "vapid_private_key: " + key_file.path() + "\nsubject: \"" + subject + "\"\nca_file: " + TestHttpsServer::ca_file() + "\n" + extra;
    return service->configure(YAML::Load(yaml), Config(logger));
  }
};

// RFC 7235 2.1: auth-scheme, then comma-separated auth-params.
struct Credentials {
  std::string scheme;
  std::map<std::string, std::string> params;
};

Credentials parse_authorization(const std::string& value) {
  Credentials credentials;
  const auto space = value.find(' ');
  credentials.scheme = value.substr(0, space);
  if (space == std::string::npos) return credentials;

  std::string rest = value.substr(space + 1);
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    std::string part = rest.substr(0, comma);
    rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);

    part.erase(0, part.find_first_not_of(' '));
    part.erase(part.find_last_not_of(' ') + 1);

    const auto eq = part.find('=');
    if (eq != std::string::npos) credentials.params[part.substr(0, eq)] = part.substr(eq + 1);
  }
  return credentials;
}

TestHttpsServer::Responder answering(unsigned status) {
  return [status](const TestHttpsServer::Request&) {
    TestHttpsServer::Reply reply;
    reply.status = status;
    return reply;
  };
}

}  // namespace

TEST(WebpushPushServiceTest, IsNamedForItsPnProviderValue) {
  Fixture fixture;
  // RFC 8599 12: the 'pn-provider' value is "webpush".
  EXPECT_EQ(fixture.service->name(), "webpush");
  EXPECT_EQ(fixture.service->kind(), "push");
}

// The verifier the tests trust has to accept the RFC's own example before it is believed
// about the driver's tokens.
TEST(WebpushPushServiceTest, VerifierAcceptsTheRfc8292Example) {
  const std::string token =
      "eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiJ9.eyJhdWQiOiJodHRwczovL3B1c2guZXhhbXBsZS5uZXQiLCJleHAiOjE0NTM1MjM3NjgsInN1YiI6Im1haWx0bzpwdXNoQGV4YW1wbGUuY29tIn0."
      "i3CYb7t4xfxCDquptFOepC9GAu_HLGkMlMuCGSK2rpiUfnK9ojFwDXb1JrErtmysazNjjvW2L9OkSSHzvoD1oA";
  const std::string k = "BA1Hxzyi1RUM1b5wjxsn7nGxAszw2u61m164i3MrAIxHF6YK5h4SDYic-dRuU_RCPCfA5aq9ojSwk5Y2EmClBPs";

  const auto point = push::jwt::base64url_decode(k);
  ASSERT_TRUE(point);
  auto key = push_test::ec_public_key(*point);
  ASSERT_TRUE(key);

  auto parsed = push_test::split(token);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->claims.at("aud").as_string(), "https://push.example.net");
  EXPECT_TRUE(push_test::verify_es256(*parsed, key));

  parsed->signing_input += "x";
  EXPECT_FALSE(push_test::verify_es256(*parsed, key));
}

TEST(WebpushPushServiceTest, AcceptsAnHttpsSubscriptionUriWithoutPnParam) {
  Fixture fixture;
  EXPECT_TRUE(fixture.service->accepts(subscription("https://push.example.net/p/JzLQ3raZJfFBR0aqvOMsLrt54w4rJUsV")));
  EXPECT_TRUE(fixture.service->accepts(subscription("https://push.example.net:8443/p/x")));
}

TEST(WebpushPushServiceTest, RefusesABindingWithPnParam) {
  Fixture fixture;
  auto notification = subscription("https://push.example.net/p/x");
  notification.param = "anything";

  // RFC 8599 12: "The value of the 'pn-param' URI parameter MUST NOT be used."
  EXPECT_FALSE(fixture.service->accepts(notification));
}

TEST(WebpushPushServiceTest, RefusesAPnPridThatIsNotAnHttpsUri) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->accepts(subscription("")));
  EXPECT_FALSE(fixture.service->accepts(subscription("http://push.example.net/p/x")));
  EXPECT_FALSE(fixture.service->accepts(subscription("https://")));
  EXPECT_FALSE(fixture.service->accepts(subscription("dGhpcyBpcyBhbiBhcG5zIHRva2Vu")));
}

// RFC 6454 6.1: scheme and host lowercased, the port only when it is not the default.
TEST(WebpushPushServiceTest, OriginIsTheRfc6454Serialisation) {
  EXPECT_EQ(WebpushPushService::origin("https://Push.Example.NET/p/x?y=z"), "https://push.example.net");
  EXPECT_EQ(WebpushPushService::origin("HTTPS://push.example.net:443/p/x"), "https://push.example.net");
  EXPECT_EQ(WebpushPushService::origin("https://push.example.net:8443/p/x"), "https://push.example.net:8443");
  EXPECT_EQ(WebpushPushService::origin("https://[2001:db8::1]/p"), "https://[2001:db8::1]");
  EXPECT_EQ(WebpushPushService::origin("http://push.example.net/p"), "");
}

TEST(WebpushPushServiceTest, ConfiguresWithAP256KeyAndASubject) {
  Fixture fixture;
  EXPECT_TRUE(fixture.configure());
  EXPECT_TRUE(fixture.configure("", "https://example.com/contact"));
  EXPECT_TRUE(fixture.service->health());
}

TEST(WebpushPushServiceTest, RefusesConfigurationWithoutASection) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->configure(YAML::Node(), Config(fixture.logger)));
  EXPECT_FALSE(fixture.service->health());
  EXPECT_FALSE(fixture.logger->lines(loggers::LogLevel::ERROR).empty());
}

TEST(WebpushPushServiceTest, RefusesConfigurationWithoutAKey) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->configure(YAML::Load("subject: mailto:ops@example.com"), Config(fixture.logger)));
  EXPECT_FALSE(fixture.logger->lines(loggers::LogLevel::ERROR).empty());
}

TEST(WebpushPushServiceTest, RefusesAKeyFileThatCannotBeRead) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->configure(YAML::Load("vapid_private_key: /nonexistent/vapid.pem\nsubject: mailto:ops@example.com"), Config(fixture.logger)));

  push_test::TempFile garbage("not a key");
  EXPECT_FALSE(fixture.service->configure(YAML::Load("vapid_private_key: " + garbage.path() + "\nsubject: mailto:ops@example.com"), Config(fixture.logger)));
  EXPECT_FALSE(fixture.logger->lines(loggers::LogLevel::ERROR).empty());
}

// RFC 8292 2: the signature MUST use ECDSA on P-256.
TEST(WebpushPushServiceTest, RefusesAKeyThatIsNotP256) {
  Fixture fixture;

  push_test::TempFile p384(push_test::private_pem(push_test::generate_ec("P-384")));
  EXPECT_FALSE(fixture.service->configure(YAML::Load("vapid_private_key: " + p384.path() + "\nsubject: mailto:ops@example.com"), Config(fixture.logger)));

  push_test::TempFile rsa(push_test::private_pem(push_test::generate_rsa()));
  EXPECT_FALSE(fixture.service->configure(YAML::Load("vapid_private_key: " + rsa.path() + "\nsubject: mailto:ops@example.com"), Config(fixture.logger)));
}

// RFC 8292 2.1: the subject is a mailto: or https: contact URI.
TEST(WebpushPushServiceTest, RefusesAMissingOrUnusableSubject) {
  Fixture fixture;
  EXPECT_FALSE(fixture.service->configure(YAML::Load("vapid_private_key: " + fixture.key_file.path()), Config(fixture.logger)));
  EXPECT_FALSE(fixture.configure("", "xmpp:ops@example.com"));
  EXPECT_FALSE(fixture.configure("", "http://example.com/contact"));
  EXPECT_FALSE(fixture.configure("", "mailto:"));
}

TEST(WebpushPushServiceTest, RefusesANegativeTtl) {
  Fixture fixture;
  EXPECT_FALSE(fixture.configure("ttl: -1"));
  EXPECT_FALSE(fixture.configure("ttl: soon"));
}

TEST(WebpushPushServiceTest, RefusesACaFileThatWillNotLoad) {
  Fixture fixture;
  const std::string yaml = "vapid_private_key: " + fixture.key_file.path() + "\nsubject: mailto:ops@example.com\nca_file: /nonexistent/ca.pem";
  EXPECT_FALSE(fixture.service->configure(YAML::Load(yaml), Config(fixture.logger)));
}

// RFC 8599 5.6.1.1 and 8.3: +sip.vapid carries the key, in the form RFC 8292 3.2 gives "k".
TEST(WebpushPushServiceTest, AdvertisesTheVapidPublicKey) {
  Fixture fixture;
  EXPECT_TRUE(fixture.service->capabilities().empty());

  ASSERT_TRUE(fixture.configure());
  const auto capabilities = fixture.service->capabilities();
  ASSERT_EQ(capabilities.size(), 1u);
  EXPECT_EQ(capabilities[0].first, "+sip.vapid");

  const auto point = push::jwt::base64url_decode(capabilities[0].second);
  ASSERT_TRUE(point);
  ASSERT_EQ(point->size(), 65u);
  EXPECT_EQ(static_cast<unsigned char>((*point)[0]), 0x04);
  EXPECT_EQ(*point, push_test::uncompressed_point(fixture.key));
  EXPECT_EQ(capabilities[0].second.find_first_of("+/="), std::string::npos);
}

TEST(WebpushPushServiceTest, PostsAnEmptyMessageWithTtlUrgencyAndVapid) {
  TestHttpsServer server(answering(201));
  Fixture fixture;
  ASSERT_TRUE(fixture.configure());

  const auto before = push_test::now_seconds();
  const auto status = push_test::send(*fixture.service, subscription(server.url("/p/JzLQ3raZJfFBR0aqvOMsLrt54w4rJUsV")));
  ASSERT_TRUE(status.ok) << status.error;

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1u);
  const auto& request = requests[0];

  // RFC 8030 5: a push message is a POST to the subscription's push resource.
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.target, "/p/JzLQ3raZJfFBR0aqvOMsLrt54w4rJUsV");

  // RFC 8599 12: no payload, so nothing to encrypt and no Content-Encoding.
  EXPECT_TRUE(request.body.empty());
  EXPECT_EQ(request.header("Content-Length"), "0");
  EXPECT_EQ(request.header_count("Content-Encoding"), 0u);

  // RFC 8030 5.2 requires TTL; 5.3 Urgency, high for a call.
  EXPECT_EQ(request.header("TTL"), "60");
  EXPECT_EQ(request.header("Urgency"), "high");

  // RFC 8292 3: Authorization: vapid t=<JWT>, k=<key>.
  const auto credentials = parse_authorization(request.header("Authorization"));
  EXPECT_EQ(credentials.scheme, "vapid");
  ASSERT_EQ(credentials.params.count("t"), 1u);
  ASSERT_EQ(credentials.params.count("k"), 1u);
  EXPECT_EQ(credentials.params.at("k"), fixture.service->capabilities()[0].second);

  const auto token = push_test::split(credentials.params.at("t"));
  ASSERT_TRUE(token);

  // RFC 8292 2: a JWS signed with ES256 by the key in "k".
  EXPECT_EQ(token->header.at("alg").as_string(), "ES256");
  EXPECT_EQ(token->header.at("typ").as_string(), "JWT");
  EXPECT_EQ(token->signature.size(), 64u);
  const auto point = push::jwt::base64url_decode(credentials.params.at("k"));
  ASSERT_TRUE(point);
  EXPECT_TRUE(push_test::verify_es256(*token, push_test::ec_public_key(*point)));

  // "aud" is the origin of the push resource, "exp" no more than 24 hours on, "sub" the contact.
  EXPECT_EQ(token->claims.at("aud").as_string(), "https://127.0.0.1:" + std::to_string(server.port()));
  const auto exp = token->claims.at("exp").to_number<std::int64_t>();
  EXPECT_GT(exp, before);
  EXPECT_LE(exp, push_test::now_seconds() + 24 * 3600);
  EXPECT_EQ(token->claims.at("sub").as_string(), "mailto:ops@example.com");
}

TEST(WebpushPushServiceTest, SendsTheConfiguredTtl) {
  TestHttpsServer server(answering(201));
  Fixture fixture;
  ASSERT_TRUE(fixture.configure("ttl: 30"));

  ASSERT_TRUE(push_test::send(*fixture.service, subscription(server.url("/p/1"))).ok);
  ASSERT_EQ(server.requests().size(), 1u);
  EXPECT_EQ(server.requests()[0].header("TTL"), "30");
}

// RFC 8030 5: 201 Created is the push service accepting the message, and nothing else is.
TEST(WebpushPushServiceTest, AnythingButCreatedIsAFailureCarryingTheStatus) {
  for (const unsigned code : {200u, 400u, 403u, 404u, 410u, 413u, 429u, 500u}) {
    TestHttpsServer server(answering(code));
    Fixture fixture;
    ASSERT_TRUE(fixture.configure());

    const auto status = push_test::send(*fixture.service, subscription(server.url("/p/1")));
    EXPECT_FALSE(status.ok) << code;
    EXPECT_NE(status.error.find(std::to_string(code)), std::string::npos) << status.error;
  }
}

TEST(WebpushPushServiceTest, RefusesToSendWhatItDoesNotAccept) {
  TestHttpsServer server(answering(201));
  Fixture fixture;
  ASSERT_TRUE(fixture.configure());

  auto notification = subscription(server.url("/p/1"));
  notification.param = "project";
  EXPECT_FALSE(push_test::send(*fixture.service, notification).ok);
  EXPECT_TRUE(server.requests().empty());
}

TEST(WebpushPushServiceTest, RefusesToSendUnconfigured) {
  TestHttpsServer server(answering(201));
  Fixture fixture;

  EXPECT_FALSE(push_test::send(*fixture.service, subscription(server.url("/p/1"))).ok);
  EXPECT_TRUE(server.requests().empty());
}

TEST(WebpushPushServiceTest, ReportsAnUnreachablePushService) {
  Fixture fixture;
  ASSERT_TRUE(fixture.configure());

  // A port nothing listens on: the server is gone before the push.
  std::string url;
  {
    TestHttpsServer server(answering(201));
    url = server.url("/p/1");
  }

  EXPECT_FALSE(push_test::send(*fixture.service, subscription(url)).ok);
}
