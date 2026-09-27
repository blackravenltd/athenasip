//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/auth_api.h"
#include "api/provisioning_api.h"
#include "api/router.h"
#include "api/sessions.h"
#include "config.h"
#include "datastores/memory_datastore.h"
#include "types/password.h"
#include "types/url.h"
#include "types/user.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

constexpr const char* kPassword = "correct horse battery staple";
constexpr std::uint32_t kIterations = 1000;

struct Response {
  unsigned status = 0;
  std::string body;

  boost::json::value json() const {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(body, ec);
    return ec ? boost::json::value() : parsed;
  }
};

// A store that cannot resolve a session, for the difference between "you may not" and
// "I could not ask". Everything else it does works, so a route that needs no store still
// answers.
class NoSessionsDatastore : public datastores::MemoryDatastore {
 public:
  using MemoryDatastore::MemoryDatastore;

  void session_get(plugins::Executor on, std::string, plugins::Handler<std::shared_ptr<types::Session>> handler) override {
    boost::asio::post(on, [handler] { handler(plugins::Result<std::shared_ptr<types::Session>>::failure("the store is down")); });
  }
};

// The whole API behind one router, because what is under test is which credentials reach
// which routes and that is decided in the router rather than in any handler.
struct RolesFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::ProvisioningAPI> provisioning;
  std::shared_ptr<api::AuthAPI> auth;

  explicit RolesFixture(std::shared_ptr<datastores::MemoryDatastore> with_driver = nullptr) {
    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";
    config->udp_enable = true;
    config->udp_address = "0.0.0.0";
    config->udp_port = 5060;

    datastore = with_driver ? with_driver : std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    config->http_api_tokens = {
        Config::ApiToken{"admin-token", {"admin"}},
        Config::ApiToken{"client-token", {"client"}},
        Config::ApiToken{"scopeless-token", {}},
    };

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);

    auto bearer = std::make_shared<api::BearerAuth>(config->http_api_tokens);
    router = std::make_shared<api::Router>(bearer);

    auto sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);

    provisioning = std::make_shared<api::ProvisioningAPI>(logger, datastore, admin->executor(), config, "0.0.0-test");
    provisioning->register_routes(*router);

    auth = std::make_shared<api::AuthAPI>(logger, sessions);
    auth->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
  }

  ~RolesFixture() { admin->stop(); }

  void add_user(const std::string& username, std::vector<std::string> roles, bool disabled = false) {
    auto user = std::make_shared<types::User>();
    user->username = username;
    user->roles = std::move(roles);
    user->password_hash = types::Password::hash(kPassword, kIterations);
    user->disabled = disabled;
    user->created_at = std::time(nullptr);

    EXPECT_TRUE(store->user_create(user));
  }

  Response request(http::verb method, const std::string& target, const std::string& token, const std::string& body = "") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);

    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + token);
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

  Response get(const std::string& target, const std::string& token) { return request(http::verb::get, target, token); }

  // A user's own credential, which before roles on the routes could reach nothing.
  std::string login(const std::string& username) {
    boost::json::object body;
    body["username"] = username;
    body["password"] = kPassword;

    auto response = request(http::verb::post, "/api/v1/auth/login", "", boost::json::serialize(body));
    EXPECT_EQ(response.status, 200u);

    return std::string(response.json().at("token").as_string());
  }
};

}  // namespace

// --- What a session token can now reach ---

TEST(RouteRolesTest, AUsersOwnTokenReachesARouteItHoldsTheRoleFor) {
  RolesFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  // The point of the whole step: before this, only a configuration token could get past
  // the router, so a logged-in administrator could do nothing at all.
  EXPECT_EQ(f.get("/api/v1/realms", f.login("tom")).status, 200u);
}

TEST(RouteRolesTest, AUserWithoutTheRoleIsForbiddenRatherThanUnauthorised) {
  RolesFixture f;
  f.add_user("tom", {types::roles::view_cluster_status});

  auto response = f.get("/api/v1/realms", f.login("tom"));

  // 403, not 401: this caller is who it says it is, and may not do this. A 401 would tell
  // it to go and log in again, which would not help.
  EXPECT_EQ(response.status, 403u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "forbidden");
}

TEST(RouteRolesTest, AUserWithNoRolesReachesNothingButCanStillAskWhoItIs) {
  RolesFixture f;
  f.add_user("tom", {});

  const auto token = f.login("tom");

  EXPECT_EQ(f.get("/api/v1/realms", token).status, 403u);
  EXPECT_EQ(f.get("/api/v1/nodes", token).status, 403u);

  // An empty role set on a route means any authenticated caller, which is what lets the
  // console tell somebody they have no permissions rather than just failing to load.
  EXPECT_EQ(f.get("/api/v1/session", token).status, 200u);
}

// --- Any of the roles admits ---

TEST(RouteRolesTest, ReadingRealmsAdmitsEitherRoleAndChangingThemDoesNot) {
  RolesFixture f;
  f.add_user("placer", {types::roles::manage_realm_subscribers});

  const auto token = f.login("placer");

  // Somebody who places accounts has to be able to discover which realms exist, which is
  // what was agreed with the console.
  EXPECT_EQ(f.get("/api/v1/realms", token).status, 200u);

  // Discovering them is not the same as changing them.
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/realms", token, "{\"name\":\"example.com\"}").status, 403u);
  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/realms/example.com", token).status, 403u);
}

TEST(RouteRolesTest, TheRolesARouteWantsAreNamedInTheRefusal) {
  RolesFixture f;
  f.add_user("tom", {});

  auto response = f.get("/api/v1/realms", f.login("tom"));

  // Role names are the API's own vocabulary and are in the documentation, so naming them
  // tells a caller how to get the access it is missing and tells an attacker nothing.
  const std::string message(response.json().at("error").at("message").as_string());
  EXPECT_NE(message.find(types::roles::manage_realms), std::string::npos);
  EXPECT_NE(message.find(types::roles::manage_realm_subscribers), std::string::npos);
}

// --- Configuration tokens, mapped to the same vocabulary ---

TEST(RouteRolesTest, TheAdminScopeStillReachesEverything) {
  RolesFixture f;

  // No deployment should break on this change: an admin token is every role there is.
  EXPECT_EQ(f.get("/api/v1/realms", "admin-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/nodes", "admin-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/registrations", "admin-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/session", "admin-token").status, 200u);
}

TEST(RouteRolesTest, TheClientScopeReachesWhatItAlwaysDidAndNoMore) {
  RolesFixture f;

  EXPECT_EQ(f.get("/api/v1/nodes", "client-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/registrations", "client-token").status, 200u);

  EXPECT_EQ(f.get("/api/v1/realms", "client-token").status, 403u);
}

TEST(RouteRolesTest, AKnownTokenHoldingNoScopesIsForbiddenNotUnauthorised) {
  RolesFixture f;

  // It is a real credential this node issued and it may do nothing, which is a different
  // answer from a token nobody has heard of.
  EXPECT_EQ(f.get("/api/v1/realms", "scopeless-token").status, 403u);
  EXPECT_EQ(f.get("/api/v1/session", "scopeless-token").status, 200u);

  EXPECT_EQ(f.get("/api/v1/realms", "not-a-token-at-all").status, 401u);
}

// --- Nothing presented, and nothing open by accident ---

TEST(RouteRolesTest, ARouteThatNamesRolesRefusesACallerWithNoCredential) {
  RolesFixture f;

  auto response = f.get("/api/v1/realms", "");
  EXPECT_EQ(response.status, 401u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "unauthorized");
}

TEST(RouteRolesTest, HealthIsTheOnlyThingThatAnswersWithoutACredential) {
  RolesFixture f;

  EXPECT_EQ(f.get("/api/v1/health", "").status, 200u);

  // Everything a load balancer does not need is closed to it.
  EXPECT_EQ(f.get("/api/v1/nodes", "").status, 401u);
  EXPECT_EQ(f.get("/api/v1/registrations", "").status, 401u);
  EXPECT_EQ(f.get("/api/v1/realms", "").status, 401u);
  EXPECT_EQ(f.get("/api/v1/session", "").status, 401u);
}

// --- A credential that stops being good ---

TEST(RouteRolesTest, ARoleTakenAwayIsGoneOnTheNextRequest) {
  RolesFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  const auto token = f.login("tom");
  ASSERT_EQ(f.get("/api/v1/realms", token).status, 200u);

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->roles = {};
  ASSERT_TRUE(f.store->user_update(user));

  // Not at the next login, which is why the session does not carry the roles.
  EXPECT_EQ(f.get("/api/v1/realms", token).status, 403u);
}

TEST(RouteRolesTest, DisablingAUserShutsEveryRouteOnIt) {
  RolesFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  const auto token = f.login("tom");
  ASSERT_EQ(f.get("/api/v1/realms", token).status, 200u);

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->disabled = true;
  ASSERT_TRUE(f.store->user_update(user));

  // 401 rather than 403: the credential itself has stopped being good.
  EXPECT_EQ(f.get("/api/v1/realms", token).status, 401u);
}

TEST(RouteRolesTest, LoggingOutStopsTheTokenReachingAnything) {
  RolesFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  const auto token = f.login("tom");
  ASSERT_EQ(f.request(http::verb::post, "/api/v1/auth/logout", token).status, 204u);

  EXPECT_EQ(f.get("/api/v1/realms", token).status, 401u);
}

// --- A store that cannot be asked ---

TEST(RouteRolesTest, AStoreThatCannotResolveASessionIs503AndNotARefusal) {
  auto logger = std::make_shared<MockLogger>();
  RolesFixture f(std::make_shared<NoSessionsDatastore>(logger, std::make_shared<types::URL>("memory://")));

  // A 401 here would send an administrator looking for their password while the real
  // problem is Redis.
  auto response = f.get("/api/v1/realms", std::string(64, 'a'));
  EXPECT_EQ(response.status, 503u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "unavailable");
}

TEST(RouteRolesTest, AConfigurationTokenStillWorksWhenTheStoreCannotBeAsked) {
  auto logger = std::make_shared<MockLogger>();
  RolesFixture f(std::make_shared<NoSessionsDatastore>(logger, std::make_shared<types::URL>("memory://")));

  // Which is the whole reason a configuration token is resolved before the store is
  // consulted: it is how somebody gets back in when the store is the broken thing.
  EXPECT_EQ(f.get("/api/v1/nodes", "admin-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/session", "admin-token").status, 200u);
}
