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

#include "../helpers/signed_in_users_helper.h"
#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "config.h"
#include "datastores/memory_datastore.h"
#include "types/url.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

struct Response {
  unsigned status = 0;
  std::string body;

  boost::json::value json() const {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(body, ec);
    return ec ? boost::json::value() : parsed;
  }
};

// The API under test, on a port the operating system picked, with one token per scope.
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

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~ApiFixture() { admin->stop(); }

  // One request, one connection, closed afterwards: the API answers and shuts the
  // socket down, which is what the session does.
  Response request(http::verb method, const std::string& target, const std::string& token = "admin-token", const std::string& body = "") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);

    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
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

    return Response{response.result_int(), response.body()};
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

// Nothing is reachable without a token. An API that answers a request it cannot
// attribute is an API with no access control, whatever the config says.
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

// A token is not a licence for everything: the client scope reads, and provisioning is
// the admin scope's.
TEST(ProvisioningApiTest, ATokenWithoutTheScopeIsForbidden) {
  ApiFixture f;

  auto response = f.get("/api/v1/realms", "client-token");
  EXPECT_EQ(response.status, 403u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "forbidden");
}

// Health is what a container healthcheck reaches before it has any credentials.
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

// The create/update split is what lets this answer 409 rather than silently overwriting
// a realm somebody else is using.
TEST(ProvisioningApiTest, CreatingARealmTwiceIsAConflict) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto again = f.post("/api/v1/realms", R"({"name":"example.com"})");
  EXPECT_EQ(again.status, 409u);
  EXPECT_EQ(again.json().at("error").at("code").as_string(), "conflict");
}

// The secret this node mints nonces with never comes back out, whatever it was set to.
TEST(ProvisioningApiTest, TheNonceSecretIsNeverReturned) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com","nonce_secret":"do-not-leak"})");
  ASSERT_EQ(created.status, 201u);

  EXPECT_EQ(created.body.find("do-not-leak"), std::string::npos);
  EXPECT_EQ(f.get("/api/v1/realms/example.com").body.find("do-not-leak"), std::string::npos);
}

// The registration bounds are realm policy, so they are provisioned rather than
// configured per node. registration_minimum is what a 423 Interval Too Brief quotes in
// Min-Expires (RFC 3261 10.3 step 7), and it is off until somebody sets it.
TEST(ProvisioningApiTest, TheRegistrationBoundsAreProvisionedAndReturned) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_EQ(created.json().at("registration_minimum").as_int64(), 0);

  auto updated = f.put("/api/v1/realms/example.com", R"({"registration_minimum":120})");
  ASSERT_EQ(updated.status, 200u);
  EXPECT_EQ(updated.json().at("registration_minimum").as_int64(), 120);

  // A PUT that named only the minimum left the maximum alone.
  EXPECT_EQ(updated.json().at("registration_timeout").as_int64(), f.get("/api/v1/realms/example.com").json().at("registration_timeout").as_int64());

  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("registration_minimum").as_int64(), 120);
}

// A realm's behaviour section: what it does differently from the server's default. A realm
// created without one chooses nothing, and what it comes to is the server's default.
TEST(ProvisioningApiTest, ARealmChoosesNoBehaviourUntilItSays) {
  ApiFixture f;

  auto created = f.post("/api/v1/realms", R"({"name":"example.com"})");
  ASSERT_EQ(created.status, 201u);

  const auto json = created.json();
  EXPECT_TRUE(json.at("behaviour").at("media_anchor").is_null());
  EXPECT_TRUE(json.at("behaviour").at("media_profile").is_null());

  // The shipped default, because the fixture's server says nothing either.
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

// Null is how a realm goes back to inheriting, which is a different thing from setting the
// value the server happens to have today.
TEST(ProvisioningApiTest, NullPutsASettingBackToTheServerDefault) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com","behaviour":{"media_profile":"rtp","media_anchor":false}})").status, 201u);

  auto updated = f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_profile":null}})");
  ASSERT_EQ(updated.status, 200u);

  const auto json = updated.json();
  EXPECT_TRUE(json.at("behaviour").at("media_profile").is_null());
  EXPECT_FALSE(json.at("behaviour").at("media_anchor").as_bool()) << "left alone because it was not named";
}

// A setting nobody recognises, or a value that is not one of its choices, is refused and
// changes nothing: a realm behaving other than its operator thinks is the failure.
TEST(ProvisioningApiTest, AnUnreadableBehaviourIsRefusedAndChangesNothing) {
  ApiFixture f;
  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com","behaviour":{"media_profile":"webrtc"}})").status, 201u);

  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_profile":"web-rtc"}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"media_anchor":"yes"}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":{"anchor_media":true}})").status, 400u);
  EXPECT_EQ(f.put("/api/v1/realms/example.com", R"({"behaviour":"webrtc"})").status, 400u);

  EXPECT_EQ(f.get("/api/v1/realms/example.com").json().at("behaviour").at("media_profile").as_string(), "webrtc");
}

// The fields a realm had before the section existed are refused with where they went,
// rather than ignored: a client still sending them would otherwise see nothing change.
TEST(ProvisioningApiTest, TheOldMediaFieldsAreRefusedWithWhereTheyWent) {
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

// RFC 2617: the server holds HA1 and never the password. The API takes a password
// because an operator has one, and turns it into HA1 on the way in.
TEST(ProvisioningApiTest, AnAccountIsCreatedWithItsHa1ComputedHere) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto created = f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(created.status, 201u);
  EXPECT_EQ(created.json().at("uri").as_string(), "sip:alice@example.com");

  // Neither the password nor the hash of it is anything the API hands back.
  EXPECT_EQ(created.body.find("secret"), std::string::npos);
  EXPECT_EQ(created.body.find("ha1"), std::string::npos);

  // What the store holds is the HA1 of user, realm and password, which is what the
  // registrar will check a Digest response against.
  auto account = f.store->account_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(account, nullptr);
  EXPECT_EQ(account->ha1, Util::to_lower(Util::md5("alice:example.com:secret")));
}

// The realm in the path reaches the handler. This is not as obvious as it looks: the
// handler reads the parameter and moves the context into the datastore call in the same
// expression, and a parameter read by reference points into a map that has already been
// moved from. It cost a 404 that said "no such realm: " with nothing after the colon,
// on Linux, from code that worked on macOS.
TEST(ProvisioningApiTest, ThePathParameterReachesTheHandler) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto response = f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(response.status, 201u);

  auto listed = f.get("/api/v1/realms/example.com/accounts");
  ASSERT_EQ(listed.status, 200u);

  const auto accounts = listed.json();
  ASSERT_EQ(accounts.as_array().size(), 1u);
  EXPECT_EQ(accounts.as_array()[0].at("realm").as_string(), "example.com");
}

TEST(ProvisioningApiTest, AnAccountInARealmThatDoesNotExistIs404) {
  ApiFixture f;

  auto response = f.post("/api/v1/realms/nowhere.example/accounts", R"({"user":"alice","password":"secret"})");
  EXPECT_EQ(response.status, 404u);

  // Naming it is what distinguishes "that realm is not here" from "the path parameter
  // never arrived", which is a 404 that looks identical until you read the message.
  EXPECT_NE(response.body.find("nowhere.example"), std::string::npos);
}

TEST(ProvisioningApiTest, AnAccountWithNoCredentialIsRefused) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto response = f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice"})");
  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "invalid_request");
}

TEST(ProvisioningApiTest, AnAccountIsDeleted) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice","password":"secret"})").status, 201u);

  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com/accounts/alice").status, 204u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com/accounts/alice").status, 404u);

  // Deleting what is not there is a 404, not a 500 and not a success.
  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com/accounts/alice").status, 404u);
}

// The id is derived from the URI rather than counted, so every node that provisions the
// same account agrees about which account a binding belongs to.
TEST(ProvisioningApiTest, AnAccountIdIsDerivedFromItsUri) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);

  auto created = f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice","password":"secret"})");
  ASSERT_EQ(created.status, 201u);

  // Parsing normalises a number that fits in an int64 to one, so the comparison asks
  // for the value rather than the representation.
  EXPECT_EQ(created.json().at("id").to_number<std::uint64_t>(), Util::stable_id("sip:alice@example.com"));
}

// Registrations are written by a REGISTER and by nothing else, so with none taken the
// list is empty rather than absent.
TEST(ProvisioningApiTest, RegistrationsAreListedForTheClientScope) {
  ApiFixture f;

  ASSERT_EQ(f.post("/api/v1/realms", R"({"name":"example.com"})").status, 201u);
  ASSERT_EQ(f.post("/api/v1/realms/example.com/accounts", R"({"user":"alice","password":"secret"})").status, 201u);

  auto response = f.get("/api/v1/registrations", "client-token");
  ASSERT_EQ(response.status, 200u);
  ASSERT_TRUE(response.json().is_array());
  EXPECT_EQ(response.json().as_array().size(), 0u);

  // A binding written the way the registrar writes one comes back with the flow it was
  // learned over.
  auto account = f.store->account_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(account, nullptr);

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  binding.flow_id = "tcp://192.0.2.10:5060";
  binding.node_id = "test-node";
  ASSERT_TRUE(f.store->account_register(account, binding, 3600));

  auto after = f.get("/api/v1/registrations", "client-token");
  ASSERT_EQ(after.status, 200u);

  const auto registrations = after.json();
  ASSERT_EQ(registrations.as_array().size(), 1u);
  EXPECT_EQ(registrations.as_array()[0].at("account").as_string(), "sip:alice@example.com");
  EXPECT_EQ(registrations.as_array()[0].at("flow_id").as_string(), "tcp://192.0.2.10:5060");
}

// What a client reads to know where else the realm is served from. One node today, and
// it has to say which one is itself.
TEST(ProvisioningApiTest, TheNodeListDescribesThisNode) {
  ApiFixture f;

  auto response = f.get("/api/v1/nodes", "client-token");
  ASSERT_EQ(response.status, 200u);

  // json() parses afresh each time, so the value has to be kept rather than reached
  // into: a reference into a temporary is a reference into nothing.
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

// --- GET /client/config ---

// A browser cannot be configured by hand and reads no YAML file, so everything it needs
// to place a call has to be fetchable over the API it is already speaking.
TEST(ProvisioningApiTest, ClientConfigSaysWhereToSignalAndWhatToUseForIce) {
  ApiFixture f;
  f.config->websocket_enable = true;
  f.config->websocket_address = "0.0.0.0";
  f.config->websocket_port = 9443;
  f.config->websocket_tls = true;
  f.config->ice_servers = {{"stun:stun.example.com:3478"}, {"turn:turn.example.com:3478"}};
  f.config->turn_shared_secret = "a shared secret";
  f.config->turn_credential_ttl = 600;

  auto response = f.get("/api/v1/client/config", "client-token");
  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();

  // Pulled out so a client does not have to know that "wss" is the answer.
  EXPECT_EQ(body.at("websocket_uri").as_string(), "wss://203.0.113.5:9443");

  const auto ice = body.at("ice_servers").as_array();
  ASSERT_EQ(ice.size(), 2u);

  // STUN has nothing to authenticate to, so it is given nothing.
  EXPECT_EQ(ice.at(0).at("urls").as_string(), "stun:stun.example.com:3478");
  EXPECT_EQ(ice.at(0).as_object().find("username"), ice.at(0).as_object().end());

  // TURN gets a credential that expires on its own.
  EXPECT_EQ(ice.at(1).at("urls").as_string(), "turn:turn.example.com:3478");
  EXPECT_FALSE(ice.at(1).at("username").as_string().empty());
  EXPECT_FALSE(ice.at(1).at("credential").as_string().empty());
  EXPECT_GT(ice.at(1).at("expires_at").as_int64(), std::time(nullptr));
}

TEST(ProvisioningApiTest, ClientConfigMintsAFreshCredentialEachTime) {
  ApiFixture f;
  f.config->ice_servers = {{"turn:turn.example.com:3478"}};
  f.config->turn_shared_secret = "a shared secret";
  f.config->turn_credential_ttl = 600;

  const auto first = f.get("/api/v1/client/config", "client-token").json();

  // The expiry is what the username is, so each request gives a credential good from when
  // it was asked for rather than from when the node started.
  //
  // The name after the colon is the signed-in user's, and has to be protocol-safe: coturn refuses a username containing a space, which it reports as 401
  // "wrong username" and then 400 Bad Request - indistinguishable from a bad secret at the
  // client. An earlier version of this put our own log prose in there and no browser could
  // allocate a relay.
  const auto username = std::string(first.at("ice_servers").at(0).at("username").as_string());
  EXPECT_EQ(username, std::to_string(first.at("ice_servers").at(0).at("expires_at").as_int64()) + ":test-status");
  EXPECT_EQ(username.find(' '), std::string::npos);
}

TEST(ProvisioningApiTest, ClientConfigWithNoTurnSecretHandsOutNoCredential) {
  ApiFixture f;
  f.config->ice_servers = {{"turn:turn.example.com:3478"}};

  auto response = f.get("/api/v1/client/config", "client-token");
  ASSERT_EQ(response.status, 200u);

  // The URL is still reported, because it is what the operator configured and hiding it
  // would be lying about the deployment. A credential that could not work is not.
  const auto server = response.json().at("ice_servers").at(0).as_object();
  EXPECT_EQ(server.at("urls").as_string(), "turn:turn.example.com:3478");
  EXPECT_EQ(server.find("username"), server.end());
}

TEST(ProvisioningApiTest, ClientConfigOffersNoWebsocketUriWhenThereIsNoSecureListener) {
  ApiFixture f;
  f.config->websocket_enable = true;
  f.config->websocket_address = "0.0.0.0";
  f.config->websocket_port = 9500;
  f.config->websocket_tls = false;

  auto response = f.get("/api/v1/client/config", "client-token");
  ASSERT_EQ(response.status, 200u);

  // A page served over https cannot open ws://, so a browser handed one would fail later
  // and further away than being told there is nothing.
  const auto body = response.json().as_object();
  EXPECT_EQ(body.find("websocket_uri"), body.end());

  // The listener is still in the transport list, because something that is not a browser
  // may want it.
  EXPECT_EQ(body.at("transports").as_array().at(0).at("transport").as_string(), "udp");
}

TEST(ProvisioningApiTest, ClientConfigNeedsACredentialLikeEverythingElse) {
  ApiFixture f;

  EXPECT_EQ(f.get("/api/v1/client/config", "").status, 401u);
}
