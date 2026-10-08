//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/provisioning_api.h"

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <string>

#include "../helpers/fake_push_service_helper.h"
#include "../helpers/signed_in_users_helper.h"
#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "api/subscriber_auth.h"
#include "config.h"
#include "datastores/memory_datastore.h"
#include "node_directory.h"
#include "types/authorization.h"
#include "types/url.h"
#include "util.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

struct Response {
  unsigned status = 0;
  std::string body;

  // Every WWW-Authenticate value, in order.
  std::vector<std::string> challenges;

  boost::json::value json() const {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(body, ec);
    return ec ? boost::json::value() : parsed;
  }
};

// The API under test, on a port the operating system picked, with the SignedInUsers credentials.
struct ApiFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  SignedInUsers signed_in;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::ProvisioningAPI> provisioning;

  ApiFixture() {
    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";
    config->udp_enable = true;
    config->udp_address = "0.0.0.0";
    config->udp_port = 5060;
    config->sip_public_address = "203.0.113.5";

    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);
    router = std::make_shared<api::Router>(bearer);
    provisioning = std::make_shared<api::ProvisioningAPI>(logger, datastore, admin->executor(), config, "0.0.0-test");
    provisioning->register_routes(*router);
    router->subscriber_auth_register(std::make_shared<api::SubscriberAuth>(logger, datastore, admin->executor()));

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~ApiFixture() { admin->stop(); }

  // One request, one connection: the API answers and closes the socket.
  Response request(http::verb method, const std::string& target, const std::string& token = "admin-token", const std::string& body = "",
                   const std::string& authorization = "") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);

    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
    if (!authorization.empty()) request.set(http::field::authorization, authorization);
    if (!body.empty()) {
      request.set(http::field::content_type, "application/json");
      request.body() = body;
    }
    request.prepare_payload();

    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);

    socket.close(ec);

    Response out{response.result_int(), response.body(), {}};
    const auto challenges = response.equal_range(http::field::www_authenticate);
    for (auto field = challenges.first; field != challenges.second; ++field) out.challenges.emplace_back(field->value());
    return out;
  }

  // As a subscriber's softphone would: ask, take the challenge for `algorithm`, answer it with qop=auth (RFC 7616
  // 3.4) and ask again.
  Response as_subscriber(http::verb method, const std::string& target, const std::string& user, const std::string& password, const std::string& body = "",
                         const std::string& algorithm = "MD5") {
    const auto first = request(method, target, "", body);
    if (first.status != 401) return first;

    std::string realm;
    std::string nonce;
    for (const auto& challenge : first.challenges) {
      if (challenge.find("algorithm=" + algorithm) == std::string::npos) continue;
      types::Authorization parsed(challenge);
      realm = parsed.fields["realm"];
      nonce = parsed.fields["nonce"];
    }
    if (nonce.empty()) return first;

    const auto hash = [&algorithm](const std::string& input) { return algorithm == "SHA-256" ? Util::sha256(input) : Util::md5(input); };
    const auto ha1 = hash(user + ":" + realm + ":" + password);
    const auto response = hash(ha1 + ":" + nonce + ":00000001:0a4f113b:auth:" + hash(std::string(http::to_string(method)) + ":" + target));

    return request(method, target, "", body,
                   "Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + target + "\", algorithm=" + algorithm +
                       ", qop=auth, nc=00000001, cnonce=\"0a4f113b\", response=\"" + response + "\"");
  }

  Response post(const std::string& target, const std::string& body, const std::string& token = "admin-token") {
    return request(http::verb::post, target, token, body);
  }

  Response put(const std::string& target, const std::string& body, const std::string& token = "admin-token") {
    return request(http::verb::put, target, token, body);
  }

  Response get(const std::string& target, const std::string& token = "admin-token") { return request(http::verb::get, target, token); }
};

}  // namespace

namespace {

// Alice, a subscriber in example.com with the password "secret", made through the API as an operator would.
void seed_alice(ApiFixture& f) {
  if (!f.store->realm_get_by_name("example.com")) ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})").status, 201u);
}

Response alice_config(ApiFixture& f) { return f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "secret"); }

}  // namespace

// Nothing is reachable without a token.
TEST(ProvisioningApiTest, ARequestWithNoTokenIsRefused) {
  ApiFixture f;

  auto response = f.get("/api/v1/realms", "");
  EXPECT_EQ(response.status, 401u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "unauthorized");
}

TEST(ProvisioningApiTest, AnUnknownTokenIsRefused) {
  ApiFixture f;

  EXPECT_EQ(f.get("/api/v1/realms", "not-a-token").status, 401u);
}

// A credential without the route's role is forbidden.
TEST(ProvisioningApiTest, ACredentialWithoutTheRoleIsForbidden) {
  ApiFixture f;

  auto response = f.get("/api/v1/realms", "client-token");
  EXPECT_EQ(response.status, 403u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "forbidden");
}

// Health needs no credential: a container healthcheck has none.
TEST(ProvisioningApiTest, HealthNeedsNoToken) {
  ApiFixture f;

  auto response = f.get("/api/v1/health", "");
  ASSERT_EQ(response.status, 200u);
  EXPECT_EQ(response.json().at("status").as_string(), "ok");
  EXPECT_EQ(response.json().at("node").as_string(), "test-node");
}

TEST(ProvisioningApiTest, ARealmIsCreatedAndListed) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_EQ(created.json().at("name").as_string(), "example.com");

  auto listed = f.get("/api/v1/realms");
  ASSERT_EQ(listed.status, 200u);

  const auto realms = listed.json();
  ASSERT_TRUE(realms.is_array());
  ASSERT_EQ(realms.as_array().size(), 1u);
  EXPECT_EQ(realms.as_array()[0].at("name").as_string(), "example.com");
}

// Creating an existing realm is a 409, not an overwrite.
TEST(ProvisioningApiTest, CreatingARealmTwiceIsAConflict) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto again = f.post("/api/v1/realms", R"({"name":"example.com"})");
  EXPECT_EQ(again.status, 409u);
  EXPECT_EQ(again.json().at("error").at("code").as_string(), "conflict");
}

// The realm's nonce secret is never returned.
TEST(ProvisioningApiTest, TheNonceSecretIsNeverReturned) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com","nonce_secret":"do-not-leak"})");
  ASSERT_EQ(created.status, 201u);

  EXPECT_EQ(created.body.find("do-not-leak"), std::string::npos);
  EXPECT_EQ(f.get("/api/v1/realms/example.com").body.find("do-not-leak"), std::string::npos);
}

// The registration bounds are realm policy, so they are provisioned. registration_minimum is what a 423 quotes
// in Min-Expires (RFC 3261 10.3 step 7), and is off by default.
TEST(ProvisioningApiTest, TheRegistrationBoundsAreProvisionedAndReturned) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_EQ(created.json().at("registration_minimum").as_int64(), 0);

  auto updated = f.put("/api/v1/realms/example.com", R"({"registration_minimum":120})");
  ASSERT_EQ(updated.status, 200u);
  EXPECT_EQ(updated.json().at("registration_minimum").as_int64(), 120);

  // A PUT that names only the minimum leaves the maximum alone.
  EXPECT_EQ(updated.json().at("registration_timeout").as_int64(), f.get("/api/v1/realms/example.com").json().at("registration_timeout").as_int64());

  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("registration_minimum").as_int64(), 120);
}

// A realm's behaviour section holds what it does differently from the server's default; empty inherits it all.
TEST(ProvisioningApiTest, ARealmChoosesNoBehaviourUntilItSays) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com"})");
  ASSERT_EQ(created.status, 201u);

  const auto json = created.json();
  EXPECT_TRUE(json.at("behaviour").at("media_anchor").is_null());
  EXPECT_TRUE(json.at("behaviour").at("media_profile").is_null());

  // The shipped default: the fixture's server sets nothing either.
  EXPECT_TRUE(json.at("behaviour_effective").at("media_anchor").as_bool());
  EXPECT_EQ(json.at("behaviour_effective").at("media_profile").as_string(), "mirror");
}

TEST(ProvisioningApiTest, ARealmOverridesOneSettingAndInheritsTheOther) {
  ApiFixture f;
  f.config->behaviour.anchor = false;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto updated = f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_profile":"webrtc"}})");
  ASSERT_EQ(updated.status, 200u) << updated.body;

  const auto json = f.get("/api/v1/realms/example.com").json();
  EXPECT_EQ(json.at("behaviour").at("media_profile").as_string(), "webrtc");
  EXPECT_TRUE(json.at("behaviour").at("media_anchor").is_null());

  EXPECT_EQ(json.at("behaviour_effective").at("media_profile").as_string(), "webrtc");
  EXPECT_FALSE(json.at("behaviour_effective").at("media_anchor").as_bool()) << "the server's, inherited";

  // What inheriting would come to, for the setting the realm did choose.
  EXPECT_EQ(json.at("behaviour_default").at("media_profile").as_string(), "mirror");
  EXPECT_FALSE(json.at("behaviour_default").at("media_anchor").as_bool());
}

// Null puts a setting back to inheriting, which is not the same as setting the server's current value.
TEST(ProvisioningApiTest, NullPutsASettingBackToTheServerDefault) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com","behaviour":{"media_profile":"rtp","media_anchor":false}})").status, 201u);

  auto updated = f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_profile":null}})");
  ASSERT_EQ(updated.status, 200u);

  const auto json = updated.json();
  EXPECT_TRUE(json.at("behaviour").at("media_profile").is_null());
  EXPECT_FALSE(json.at("behaviour").at("media_anchor").as_bool()) << "left alone because it was not named";
}

// qualify_interval is off by default; an out-of-range or non-numeric value is refused and changes nothing.
TEST(ProvisioningApiTest, ARealmSetsHowOftenItsClientsAreQualified) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto json = f.get("/api/v1/realms/example.com").json();
  EXPECT_TRUE(json.at("behaviour").at("qualify_interval").is_null());
  EXPECT_EQ(json.at("behaviour_effective").at("qualify_interval").as_int64(), 0) << "off, as shipped";
  EXPECT_EQ(json.at("behaviour_default").at("qualify_interval").as_int64(), 0);

  auto updated = f.put("/api/v1/realms/example.com", R"({"behaviour":{"qualify_interval":60}})");
  ASSERT_EQ(updated.status, 200u) << updated.body;
  EXPECT_EQ(updated.json().at("behaviour").at("qualify_interval").as_int64(), 60);
  EXPECT_EQ(updated.json().at("behaviour_effective").at("qualify_interval").as_int64(), 60);

  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"qualify_interval":2}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"qualify_interval":-60}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"qualify_interval":"60"}})").status, 400u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("behaviour").at("qualify_interval").as_int64(), 60);

  ASSERT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"qualify_interval":null}})").status, 200u);
  EXPECT_TRUE(f.get("/api/v1/realms/example.com").json().at("behaviour").at("qualify_interval").is_null());
}

TEST(ProvisioningApiTest, ARealmSetsWhetherContactsAreRewritten) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto json = f.get("/api/v1/realms/example.com").json();
  EXPECT_TRUE(json.at("behaviour").at("rewrite_contact").is_null());
  EXPECT_FALSE(json.at("behaviour_effective").at("rewrite_contact").as_bool());
  EXPECT_FALSE(json.at("behaviour_default").at("rewrite_contact").as_bool());

  auto updated = f.put("/api/v1/realms/example.com", R"({"behaviour":{"rewrite_contact":true}})");
  ASSERT_EQ(updated.status, 200u) << updated.body;
  EXPECT_TRUE(updated.json().at("behaviour_effective").at("rewrite_contact").as_bool());

  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"rewrite_contact":"yes"}})").status, 400u);
  EXPECT_TRUE(f.get("/api/v1/realms/example.com").json().at("behaviour").at("rewrite_contact").as_bool());
}

TEST(ProvisioningApiTest, AnUnreadableBehaviourIsRefusedAndChangesNothing) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com","behaviour":{"media_profile":"webrtc"}})").status, 201u);

  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_profile":"web-rtc"}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_anchor":"yes"}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"anchor_media":true}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":"webrtc"})").status, 400u);

  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("behaviour").at("media_profile").as_string(), "webrtc");
}

// Media settings outside behaviour are refused with a pointer to it, not silently ignored.
TEST(ProvisioningApiTest, AMediaFieldOutsideBehaviourIsRefusedWithAPointerToIt) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto refused = f.put("/api/v1/realms/example.com", R"({"media_profiles":"webrtc"})");
  EXPECT_EQ(refused.status, 400u);
  EXPECT_NE(refused.body.find("behaviour"), std::string::npos) << refused.body;
}

TEST(ProvisioningApiTest, AnUnknownRealmIs404) {
  ApiFixture f;

  auto response = f.get("/api/v1/realms/nowhere.example");
  EXPECT_EQ(response.status, 404u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "not_found");
}

// RFC 2617: the server holds HA1, never the password. The API takes a password and stores its HA1.
TEST(ProvisioningApiTest, ASubscriberIsCreatedWithItsHa1ComputedHere) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto created = f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_EQ(created.json().at("uri").as_string(), "sip:alice@example.com");

  // Neither the password nor its hash is returned.
  EXPECT_EQ(created.body.find("secret"), std::string::npos);
  EXPECT_EQ(created.body.find("ha1"), std::string::npos);

  // The store holds the HA1 of user, realm and password, which the registrar checks a Digest response against.
  auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(subscriber, nullptr);
  EXPECT_EQ(subscriber->ha1, Util::to_lower(Util::md5("alice:example.com:secret")));
}

// A subscriber's behaviour section has one setting, what the endpoint is. Empty takes the realm's.
TEST(ProvisioningApiTest, ASubscriberSaysWhatItsEndpointIs) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto created = f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_TRUE(created.json().at("behaviour").at("media_profile").is_null());

  auto updated = f.put("/api/v1/realms/example.com/subscribers/alice", R"({"behaviour":{"media_profile":"webrtc"}})");
  ASSERT_EQ(updated.status, 200u) << updated.body;
  EXPECT_EQ(updated.json().at("behaviour").at("media_profile").as_string(), "webrtc");

  auto stored = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->media_profile, types::MediaPolicy::Profiles::WebRtc);

  ASSERT_EQ(f.put("/api/v1/realms/example.com/subscribers/alice", R"({"behaviour":{"media_profile":null}})").status, 200u);
  stored = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  EXPECT_FALSE(stored->media_profile.has_value());
}

// Anchoring is a realm setting and an unknown profile is an error. Both are refused and change nothing,
// password included.
TEST(ProvisioningApiTest, AnUnreadableSubscriberBehaviourChangesNothing) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret","behaviour":{"media_profile":"rtp"}})").status, 201u);

  EXPECT_EQ(f.put("/api/v1/realms/example.com/subscribers/alice", R"({"password":"other","behaviour":{"media_anchor":false}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com/subscribers/alice", R"({"behaviour":{"media_profile":"web-rtc"}})").status, 400u);
  EXPECT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"bob","password":"secret","behaviour":{"media_profile":7}})").status, 400u);

  auto stored = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->media_profile, types::MediaPolicy::Profiles::PlainRtp);
  EXPECT_EQ(stored->ha1, Util::to_lower(Util::md5("alice:example.com:secret")));
  EXPECT_EQ(f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:bob@example.com")), nullptr);
}

// The realm in the path reaches the handler. The handler must copy the parameter before moving the context
// into the datastore call: a reference would point into a moved-from map.
TEST(ProvisioningApiTest, ThePathParameterReachesTheHandler) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto response = f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(response.status, 201u);

  auto listed = f.get("/api/v1/realms/example.com/subscribers");
  ASSERT_EQ(listed.status, 200u);

  const auto subscribers = listed.json();
  ASSERT_EQ(subscribers.as_array().size(), 1u);
  EXPECT_EQ(subscribers.as_array()[0].at("realm").as_string(), "example.com");
}

TEST(ProvisioningApiTest, ASubscriberInARealmThatDoesNotExistIs404) {
  ApiFixture f;

  auto response = f.post("/api/v1/realms/nowhere.example/subscribers", R"({"user":"alice","password":"secret"})");
  EXPECT_EQ(response.status, 404u);

  // Naming the realm distinguishes "not here" from "the path parameter never arrived".
  EXPECT_NE(response.body.find("nowhere.example"), std::string::npos);
}

TEST(ProvisioningApiTest, ASubscriberWithNoCredentialIsRefused) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto response = f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice"})");
  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "invalid_request");
}

TEST(ProvisioningApiTest, ASubscriberIsDeleted) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})").status, 201u);

  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com/subscribers/alice").status, 204u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com/subscribers/alice").status, 404u);

  // Deleting what is not there is a 404.
  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com/subscribers/alice").status, 404u);
}

// Deleting a realm deletes its subscribers and their registrations.
TEST(ProvisioningApiTest, DeletingARealmDeletesItsSubscribersAndRegistrations) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})").status, 201u);

  auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(subscriber, nullptr);
  ASSERT_TRUE(f.store->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060"), 3600, ""));

  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com").status, 204u);
  EXPECT_EQ(f.get("/api/v1/registrations", "client-token").json().as_array().size(), 0u);

  // A realm made again under the same name starts empty.
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com/subscribers").json().as_array().size(), 0u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com/subscribers/alice").status, 404u);
}

// The id is derived from the URI, so every node agrees which subscriber a binding belongs to.
TEST(ProvisioningApiTest, ASubscriberIdIsDerivedFromItsUri) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto created = f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(created.status, 201u);

  // Compare by value: parsing may hold the number as an int64.
  EXPECT_EQ(created.json().at("id").to_number<std::uint64_t>(), Util::stable_id("sip:alice@example.com"));
}

// With no REGISTER taken, the list is empty rather than absent.
TEST(ProvisioningApiTest, RegistrationsAreListedForViewClusterStatus) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})").status, 201u);

  auto response = f.get("/api/v1/registrations", "client-token");
  ASSERT_EQ(response.status, 200u);
  ASSERT_TRUE(response.json().is_array());
  EXPECT_EQ(response.json().as_array().size(), 0u);

  // A binding written as the registrar writes one comes back with the flow it was learned over.
  auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(subscriber, nullptr);

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  binding.flow_id = "tcp://192.0.2.10:5060";
  binding.node_id = "test-node";
  ASSERT_TRUE(f.store->subscriber_register(subscriber, binding, 3600));

  auto after = f.get("/api/v1/registrations", "client-token");
  ASSERT_EQ(after.status, 200u);

  const auto registrations = after.json();
  ASSERT_EQ(registrations.as_array().size(), 1u);
  EXPECT_EQ(registrations.as_array()[0].at("subscriber").as_string(), "sip:alice@example.com");
  EXPECT_EQ(registrations.as_array()[0].at("flow_id").as_string(), "tcp://192.0.2.10:5060");
}

// RFC 8599 section 13: a push binding is listed without its pn-* parameters; the token is the client's and this
// node's, not the reader's.
TEST(ProvisioningApiTest, RegistrationsAreListedWithoutPushTokens) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret"})").status, 201u);

  auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(subscriber, nullptr);

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060;transport=tcp;pn-provider=fcm;pn-param=project;pn-prid=secret-token");
  binding.push = true;
  ASSERT_TRUE(f.store->subscriber_register(subscriber, binding, 3600));

  const auto registrations = f.get("/api/v1/registrations", "client-token").json();
  ASSERT_EQ(registrations.as_array().size(), 1u);
  EXPECT_EQ(registrations.as_array()[0].at("contact").as_string(), "sip:alice@192.0.2.10:5060;transport=tcp");
}

// The node list says where the realm is served from and which node is this one.
TEST(ProvisioningApiTest, TheNodeListDescribesThisNode) {
  ApiFixture f;

  auto response = f.get("/api/v1/nodes", "client-token");
  ASSERT_EQ(response.status, 200u);

  // json() parses afresh each call, so keep the value: a reference into the temporary would dangle.
  const auto body = response.json();
  ASSERT_EQ(body.as_array().size(), 1u);

  const auto& node = body.as_array()[0];
  EXPECT_EQ(node.at("id").as_string(), "test-node");
  EXPECT_TRUE(node.at("self").as_bool());

  ASSERT_EQ(node.at("transports").as_array().size(), 1u);
  const auto& transport = node.at("transports").as_array()[0];
  EXPECT_EQ(transport.at("transport").as_string(), "udp");

  // sip.public_address, not the wildcard it is bound to: a client cannot dial 0.0.0.0.
  EXPECT_EQ(transport.at("address").as_string(), "203.0.113.5");
}

// The other nodes, from what each said on the event bus: where they listen, their description, and whether
// that is current.
TEST(ProvisioningApiTest, TheNodeListIncludesWhatTheOtherNodesSaid) {
  ApiFixture f;
  auto directory = std::make_shared<NodeDirectory>();
  f.provisioning->nodes_register(directory, std::chrono::seconds(30));

  directory->observe(
      "nodes/node-b/status",
      R"({"status":"ok","node":"node-b","version":"1.2.3","at":"2026-10-02T10:00:00Z","transports":[{"transport":"tls","address":"198.51.100.7","port":5061,"uri":"sips:198.51.100.7:5061;transport=tls"}]})");
  directory->observe("nodes/test-node/status", R"({"status":"ok","node":"test-node","version":"0.0.0-test","transports":[]})");

  auto response = f.get("/api/v1/nodes", "client-token");
  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();
  const auto& nodes = body.as_array();
  ASSERT_EQ(nodes.size(), 2u) << "this node once, from its own config, and node-b";

  EXPECT_EQ(nodes[0].at("id").as_string(), "test-node");
  EXPECT_TRUE(nodes[0].at("self").as_bool());
  EXPECT_EQ(nodes[0].at("status").as_string(), "ok");

  const auto& other = nodes[1];
  EXPECT_EQ(other.at("id").as_string(), "node-b");
  EXPECT_FALSE(other.at("self").as_bool());
  EXPECT_EQ(other.at("status").as_string(), "ok");
  EXPECT_FALSE(other.at("stale").as_bool());
  EXPECT_EQ(other.at("version").as_string(), "1.2.3");
  EXPECT_EQ(other.at("transports").as_array()[0].at("uri").as_string(), "sips:198.51.100.7:5061;transport=tls");
}

// The browser bootstrap lists where a client can go: this node first, then the others that are up, with their
// secure WebSocket URIs in that order. A node that is down or stale is not listed.
TEST(ProvisioningApiTest, SubscriberConfigListsTheNodesAClientCanUse) {
  ApiFixture f;
  f.config->websocket_enable = true;
  f.config->websocket_tls = true;
  f.config->websocket_address = "0.0.0.0";
  f.config->websocket_port = 8089;

  auto directory = std::make_shared<NodeDirectory>();
  f.provisioning->nodes_register(directory, std::chrono::seconds(30));

  const auto wss = [](const std::string& address) {
    return R"([{"transport":"wss","address":")" + address + R"(","port":8089,"uri":"sips:)" + address + R"(:8089;transport=wss"}])";
  };
  directory->observe("nodes/node-b/status", R"({"status":"ok","node":"node-b","transports":)" + wss("198.51.100.7") + "}");
  directory->observe("nodes/node-c/status", R"({"status":"down","node":"node-c","transports":)" + wss("198.51.100.8") + "}");

  seed_alice(f);
  auto response = alice_config(f);
  ASSERT_EQ(response.status, 200u) << response.body;
  const auto body = response.json();

  const auto& nodes = body.at("nodes").as_array();
  ASSERT_EQ(nodes.size(), 2u);
  EXPECT_EQ(nodes[0].at("id").as_string(), "test-node");
  EXPECT_EQ(nodes[1].at("id").as_string(), "node-b");

  const auto& uris = body.at("websocket_uris").as_array();
  ASSERT_EQ(uris.size(), 2u);
  EXPECT_EQ(uris[0].as_string(), "wss://203.0.113.5:8089");
  EXPECT_EQ(uris[1].as_string(), "wss://198.51.100.7:8089");
}

TEST(ProvisioningApiTest, AnUnknownEndpointIs404AndAWrongMethodIs405) {
  ApiFixture f;

  EXPECT_EQ(f.get("/api/v1/nothing").status, 404u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/health", "").status, 405u);
}

TEST(ProvisioningApiTest, ABodyThatIsNotJsonIsRefused) {
  ApiFixture f;

  auto response = f.post("/api/v1/realms", "not json at all");
  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "invalid_json");
}

// --- GET /subscriber/{realm}/config ---

// Named, a realm says what it expects of a client registering in it: the lifetime it grants and the shortest it
// takes (RFC 3261 10.3 step 7), and how many RFC 5626 flows to keep, one per node up to two (section 4.2).
TEST(ProvisioningApiTest, SubscriberConfigSaysWhatARealmExpects) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  auto realm = f.store->realm_get_by_name("example.com");
  realm->registration_timeout = 1800;
  realm->registration_minimum = 60;
  f.store->realm_update(realm);
  seed_alice(f);

  auto response = alice_config(f);
  ASSERT_EQ(response.status, 200u);

  const auto config = response.json();
  const auto& expects = config.at("realm").as_object();
  EXPECT_EQ(expects.at("name").as_string(), "example.com");
  EXPECT_EQ(expects.at("registration").at("expires").to_number<std::uint64_t>(), 1800u);
  EXPECT_EQ(expects.at("registration").at("minimum").to_number<std::uint64_t>(), 60u);
  EXPECT_EQ(expects.at("outbound").at("flows").to_number<std::uint64_t>(), 1u);
  EXPECT_TRUE(expects.at("push").as_array().empty());
}

// RFC 8599: the push services the node runs, with what a client needs before it can subscribe (the VAPID key for
// webpush, 4.1.1) and the shortest registration push accepts (5.6.1.1).
TEST(ProvisioningApiTest, SubscriberConfigListsThePushServices) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  f.provisioning->push_register({std::make_shared<FakePushService>("webpush", std::vector<std::pair<std::string, std::string>>{{"+sip.vapid", "BKey"}})});
  seed_alice(f);

  const auto config = alice_config(f).json();

  const auto& push = config.at("realm").at("push").as_array();
  ASSERT_EQ(push.size(), 1u);
  EXPECT_EQ(push[0].at("service").as_string(), "webpush");
  EXPECT_EQ(push[0].at("vapid").as_string(), "BKey");
  EXPECT_EQ(push[0].at("minimum_expires").to_number<std::uint64_t>(), f.config->push_minimum_expiry());
}

// A realm this node does not serve is a 404, as everywhere else.
TEST(ProvisioningApiTest, SubscriberConfigForAnUnknownRealmIs404) {
  ApiFixture f;

  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/nowhere.example/config", "alice", "secret").status, 404u);
}

// Everything a browser needs to place a call is fetchable over the API.
TEST(ProvisioningApiTest, SubscriberConfigSaysWhereToSignalAndWhatToUseForIce) {
  ApiFixture f;
  f.config->websocket_enable = true;
  f.config->websocket_address = "0.0.0.0";
  f.config->websocket_port = 9443;
  f.config->websocket_tls = true;
  f.config->ice_servers = {{"stun:stun.example.com:3478"}, {"turn:turn.example.com:3478"}};
  f.config->turn_shared_secret = "a shared secret";
  f.config->turn_credential_ttl = 600;
  seed_alice(f);

  auto response = alice_config(f);
  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();

  // The WebSocket URI is given directly.
  EXPECT_EQ(body.at("websocket_uri").as_string(), "wss://203.0.113.5:9443");

  const auto ice = body.at("ice_servers").as_array();
  ASSERT_EQ(ice.size(), 2u);

  // STUN needs no credential.
  EXPECT_EQ(ice.at(0).at("urls").as_string(), "stun:stun.example.com:3478");
  EXPECT_EQ(ice.at(0).as_object().find("username"), ice.at(0).as_object().end());

  // TURN gets a credential that expires.
  EXPECT_EQ(ice.at(1).at("urls").as_string(), "turn:turn.example.com:3478");
  EXPECT_FALSE(ice.at(1).at("username").as_string().empty());
  EXPECT_FALSE(ice.at(1).at("credential").as_string().empty());
  EXPECT_GT(ice.at(1).at("expires_at").as_int64(), std::time(nullptr));
}

TEST(ProvisioningApiTest, SubscriberConfigMintsAFreshCredentialEachTime) {
  ApiFixture f;
  f.config->ice_servers = {{"turn:turn.example.com:3478"}};
  f.config->turn_shared_secret = "a shared secret";
  f.config->turn_credential_ttl = 600;

  seed_alice(f);
  const auto first = alice_config(f).json();

  // The username is the expiry, so each request gives a credential good from when it was asked for. The name
  // after the colon is the subscriber's and must be protocol-safe: coturn refuses a username with a space.
  const auto username = std::string(first.at("ice_servers").at(0).at("username").as_string());
  EXPECT_EQ(username, std::to_string(first.at("ice_servers").at(0).at("expires_at").as_int64()) + ":alice_example.com");
  EXPECT_EQ(username.find(' '), std::string::npos);
}

TEST(ProvisioningApiTest, SubscriberConfigWithNoTurnSecretHandsOutNoCredential) {
  ApiFixture f;
  f.config->ice_servers = {{"turn:turn.example.com:3478"}};
  seed_alice(f);

  auto response = alice_config(f);
  ASSERT_EQ(response.status, 200u);

  // The configured URL is still reported; a credential that could not work is not.
  const auto server = response.json().at("ice_servers").at(0).as_object();
  EXPECT_EQ(server.at("urls").as_string(), "turn:turn.example.com:3478");
  EXPECT_EQ(server.find("username"), server.end());
}

TEST(ProvisioningApiTest, SubscriberConfigOffersNoWebsocketUriWhenThereIsNoSecureListener) {
  ApiFixture f;
  f.config->websocket_enable = true;
  f.config->websocket_address = "0.0.0.0";
  f.config->websocket_port = 9500;
  f.config->websocket_tls = false;
  seed_alice(f);

  auto response = alice_config(f);
  ASSERT_EQ(response.status, 200u);

  // A page served over https cannot open ws://, so a browser is told there is no WebSocket URI.
  const auto body = response.json().as_object();
  EXPECT_EQ(body.find("websocket_uri"), body.end());

  // The listener is still in the transport list, for clients that are not browsers.
  EXPECT_EQ(body.at("transports").as_array().at(0).at("transport").as_string(), "udp");
}

// RFC 7616 3.3: without credentials the answer is 401 with a Digest challenge for the realm, SHA-256 first, with
// qop=auth.
TEST(ProvisioningApiTest, ASubscriberRouteChallengesWithDigestForItsRealm) {
  ApiFixture f;
  seed_alice(f);

  auto response = f.get("/api/v1/subscriber/example.com/config", "");

  ASSERT_EQ(response.status, 401u);
  ASSERT_EQ(response.challenges.size(), 2u);
  EXPECT_NE(response.challenges[0].find("Digest realm=\"example.com\""), std::string::npos) << response.challenges[0];
  EXPECT_NE(response.challenges[0].find("algorithm=SHA-256"), std::string::npos);
  EXPECT_NE(response.challenges[0].find("qop=\"auth\""), std::string::npos);
  EXPECT_NE(response.challenges[1].find("algorithm=MD5"), std::string::npos);
}

// The subscriber's SIP credentials sign it, under either algorithm.
TEST(ProvisioningApiTest, ASubscribersSipCredentialsOpenItsRoutes) {
  ApiFixture f;
  seed_alice(f);

  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "secret", "", "MD5").status, 200u);
  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "secret", "", "SHA-256").status, 200u);
}

// A wrong password is challenged again, as is a subscriber that does not exist, so neither can be told apart.
TEST(ProvisioningApiTest, AWrongPasswordOrAnUnknownSubscriberIsChallengedAgain) {
  ApiFixture f;
  seed_alice(f);

  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "wrong").status, 401u);
  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "mallory", "secret").status, 401u);
}

// A user's session opens no subscriber route, and a subscriber's credentials open no other route.
TEST(ProvisioningApiTest, SubscriberAndUserCredentialsDoNotCross) {
  ApiFixture f;
  seed_alice(f);

  EXPECT_EQ(f.get("/api/v1/subscriber/example.com/config", "admin-token").status, 401u);
  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/realms", "alice", "secret").status, 401u);
}

// --- GET /subscriber/{realm}/registrations, PUT /subscriber/{realm}/password ---

// A subscriber sees its own bindings, and nobody else's.
TEST(ProvisioningApiTest, ASubscriberSeesItsOwnRegistrations) {
  ApiFixture f;
  seed_alice(f);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/subscribers", R"({"user":"bob","password":"other"})").status, 201u);

  for (const auto& [user, port] : {std::pair<std::string, int>{"alice", 5060}, {"bob", 5070}}) {
    auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:" + user + "@example.com"));
    types::Location binding;
    binding.contact = std::make_shared<types::SIPUri>("sip:" + user + "@192.0.2.10:" + std::to_string(port));
    ASSERT_TRUE(f.store->subscriber_register(subscriber, binding, 3600));
  }

  auto response = f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/registrations", "alice", "secret");

  ASSERT_EQ(response.status, 200u) << response.body;
  const auto registrations = response.json().as_array();
  ASSERT_EQ(registrations.size(), 1u);
  EXPECT_EQ(registrations[0].at("contact").as_string(), "sip:alice@192.0.2.10:5060");
}

// A subscriber changes its own password: the new one opens its routes and the old one no longer does.
TEST(ProvisioningApiTest, ASubscriberChangesItsOwnPassword) {
  ApiFixture f;
  seed_alice(f);

  auto changed = f.as_subscriber(http::verb::put, "/api/v1/subscriber/example.com/password", "alice", "secret", R"({"password":"a new one"})");
  ASSERT_EQ(changed.status, 204u) << changed.body;

  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "secret").status, 401u);
  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "a new one").status, 200u);
  EXPECT_EQ(f.as_subscriber(http::verb::get, "/api/v1/subscriber/example.com/config", "alice", "a new one", "", "SHA-256").status, 200u);
}

// An empty password is refused.
TEST(ProvisioningApiTest, AnEmptyPasswordIsRefused) {
  ApiFixture f;
  seed_alice(f);

  EXPECT_EQ(f.as_subscriber(http::verb::put, "/api/v1/subscriber/example.com/password", "alice", "secret", R"({"password":""})").status, 400u);
}

// A realm and a subscriber carry attributes for scripts: stored as given, returned, replaced whole on update, and
// only an object.
TEST(ProvisioningApiTest, RealmsAndSubscribersCarryAttributesForScripts) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com","attributes":{"country":"44"}})").status, 201u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("attributes").at("country").as_string(), "44");
  EXPECT_EQ(f.store->realm_get_by_name("example.com")->attributes.at("country").as_string(), "44");

  ASSERT_EQ(
      f.post("/api/v1/realms/example.com/subscribers", R"({"user":"alice","password":"secret","attributes":{"forward_to":"sip:bob@example.com"}})").status,
      201u);
  ASSERT_EQ(f.put("/api/v1/realms/example.com/subscribers/alice", R"({"attributes":{"caller_id":"+442012345678"}})").status, 200u);

  const auto alice = f.get("/api/v1/realms/example.com/subscribers/alice").json().at("attributes").as_object();
  EXPECT_FALSE(alice.contains("forward_to")) << "replaced whole";
  EXPECT_EQ(alice.at("caller_id").as_string(), "+442012345678");

  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"attributes":"country=44"})").status, 400u);
}
