//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/auth_api.h"

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

// The auth routes on a real listener, because what is under test is a status code and a
// body, and both are decided on the way out.
struct AuthFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::AuthAPI> auth;

  AuthFixture() {
    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    config->http_api_tokens = {
        Config::ApiToken{"admin-token", {"admin"}},
        Config::ApiToken{"client-token", {"client"}},
    };

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);

    auto bearer = std::make_shared<api::BearerAuth>(config->http_api_tokens);
    router = std::make_shared<api::Router>(bearer);
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});

    // What makes a session token work on a route that names roles, rather than only on the
    // routes that ask Sessions themselves.
    bearer->sessions_register(sessions);

    auth = std::make_shared<api::AuthAPI>(logger, sessions);
    auth->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
  }

  ~AuthFixture() { admin->stop(); }

  std::shared_ptr<types::User> add_user(const std::string& username, std::vector<std::string> roles, bool disabled = false) {
    auto user = std::make_shared<types::User>();
    user->username = username;
    user->roles = std::move(roles);
    user->password_hash = types::Password::hash(kPassword, kIterations);
    user->disabled = disabled;
    user->created_at = std::time(nullptr);

    EXPECT_TRUE(store->user_create(user));
    return user;
  }

  Response request(http::verb method, const std::string& target, const std::string& token, const std::string& body) {
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

  Response login(const std::string& username, const std::string& password) {
    boost::json::object body;
    body["username"] = username;
    body["password"] = password;

    return request(http::verb::post, "/api/v1/auth/login", "", boost::json::serialize(body));
  }

  Response logout(const std::string& token) { return request(http::verb::post, "/api/v1/auth/logout", token, ""); }
  Response session(const std::string& token) { return request(http::verb::get, "/api/v1/session", token, ""); }

  // The token from a login that worked, for the tests that need one and are not about
  // getting one.
  std::string token_for(const std::string& username) {
    auto response = login(username, kPassword);
    EXPECT_EQ(response.status, 200u);
    return std::string(response.json().at("token").as_string());
  }
};

}  // namespace

// --- POST /api/v1/auth/login ---

TEST(AuthApiTest, ALoginAnswersATokenItsExpiryAndTheRolesHeld) {
  AuthFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  auto response = f.login("tom", kPassword);

  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();
  EXPECT_EQ(body.at("token").as_string().size(), 64u);
  EXPECT_EQ(body.at("roles").as_array().size(), 1u);
  EXPECT_EQ(body.at("roles").at(0).as_string(), types::roles::manage_realms);

  // Unix seconds, agreed with the console, so a client can work out how long it has left
  // without parsing a date.
  ASSERT_TRUE(body.at("expires_at").is_int64());
  EXPECT_GT(body.at("expires_at").as_int64(), std::time(nullptr));
}

TEST(AuthApiTest, LoggingInNeedsNoCredentialToStartWith) {
  AuthFixture f;
  f.add_user("tom", {});

  // The route that hands out credentials cannot require one, which is the one place in
  // this API where that is true.
  EXPECT_EQ(f.login("tom", kPassword).status, 200u);
}

TEST(AuthApiTest, AWrongPasswordAnUnknownUserAndADisabledUserGiveTheSameAnswer) {
  AuthFixture f;
  f.add_user("tom", {});
  f.add_user("sam", {}, /*disabled=*/true);

  const auto wrong = f.login("tom", "not it");
  const auto unknown = f.login("nobody", kPassword);
  const auto disabled = f.login("sam", kPassword);

  EXPECT_EQ(wrong.status, 401u);
  EXPECT_EQ(unknown.status, 401u);
  EXPECT_EQ(disabled.status, 401u);

  // Byte for byte the same, because a difference anywhere in it is a way to ask this node
  // who holds an account here.
  EXPECT_EQ(wrong.body, unknown.body);
  EXPECT_EQ(wrong.body, disabled.body);
  EXPECT_EQ(wrong.json().at("error").at("code").as_string(), "unauthorized");
}

TEST(AuthApiTest, AnEmptyPasswordIsARefusalRatherThanABadRequest) {
  AuthFixture f;
  f.add_user("tom", {});

  // Probing with an empty password learns nothing that a wrong one would not.
  EXPECT_EQ(f.login("tom", "").status, 401u);
}

TEST(AuthApiTest, AMissingFieldIsTheCallersMistake) {
  AuthFixture f;
  f.add_user("tom", {});

  auto response = f.request(http::verb::post, "/api/v1/auth/login", "", "{\"username\":\"tom\"}");

  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "invalid_request");
}

TEST(AuthApiTest, ABodyThatIsNotJsonIsABadRequest) {
  AuthFixture f;

  auto response = f.request(http::verb::post, "/api/v1/auth/login", "", "not json");

  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "invalid_json");
}

TEST(AuthApiTest, AUserWithNoRolesLogsInAndIsToldItHasNone) {
  AuthFixture f;
  f.add_user("tom", {});

  auto response = f.login("tom", kPassword);

  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().at("roles").as_array().empty());
}

TEST(AuthApiTest, ALoginDoesNotAnswer404AnyMore) {
  AuthFixture f;

  // What this step was for: the console could not get past a 404 on this path.
  EXPECT_NE(f.login("nobody", kPassword).status, 404u);
}

// --- GET /api/v1/session ---

TEST(AuthApiTest, ASessionTokenIsAUser) {
  AuthFixture f;
  auto user = f.add_user("Tom", {types::roles::view_cluster_status});
  user->display_name = "Tom Cully";
  ASSERT_TRUE(f.store->user_update(user));

  const auto token = f.token_for("tom");
  auto response = f.session(token);

  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();
  EXPECT_EQ(body.at("kind").as_string(), "user");
  EXPECT_EQ(body.at("username").as_string(), "Tom");
  EXPECT_EQ(body.at("display_name").as_string(), "Tom Cully");
  EXPECT_EQ(body.at("roles").at(0).as_string(), types::roles::view_cluster_status);
  ASSERT_TRUE(body.at("expires_at").is_int64());
}

TEST(AuthApiTest, AConfigurationTokenIsAToken) {
  AuthFixture f;

  auto response = f.session("admin-token");

  ASSERT_EQ(response.status, 200u);

  const auto body = response.json();
  EXPECT_EQ(body.at("kind").as_string(), "token");
  EXPECT_EQ(body.at("scopes").at(0).as_string(), "admin");

  // The admin scope is every role there is: the one credential on this node that can do
  // everything, which is why it lives in a file only root can read.
  EXPECT_EQ(body.at("roles").as_array().size(), types::roles::all().size());

  // No expiry. A configuration token lasts as long as it is in the file.
  EXPECT_EQ(body.as_object().find("expires_at"), body.as_object().end());
}

TEST(AuthApiTest, TheClientScopeIsOnlyWhatAClientReaches) {
  AuthFixture f;

  auto response = f.session("client-token");

  ASSERT_EQ(response.status, 200u);
  EXPECT_EQ(response.json().at("roles").as_array().size(), 1u);
  EXPECT_EQ(response.json().at("roles").at(0).as_string(), types::roles::view_cluster_status);
}

TEST(AuthApiTest, RolesOnASessionAreReadOffTheUserOnEveryRequest) {
  AuthFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  const auto token = f.token_for("tom");

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->roles = {};
  ASSERT_TRUE(f.store->user_update(user));

  // A role taken away is gone on the next request rather than at the next login, which is
  // the whole reason the session does not carry them.
  auto response = f.session(token);
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().at("roles").as_array().empty());
}

TEST(AuthApiTest, AskingWhoYouAreWithNothingIsRefused) {
  AuthFixture f;

  EXPECT_EQ(f.session("").status, 401u);
  EXPECT_EQ(f.session(std::string(64, 'a')).status, 401u);
}

TEST(AuthApiTest, DisablingAUserEndsWhatItCouldAlreadyDo) {
  AuthFixture f;
  f.add_user("tom", {});

  const auto token = f.token_for("tom");
  ASSERT_EQ(f.session(token).status, 200u);

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->disabled = true;
  ASSERT_TRUE(f.store->user_update(user));

  EXPECT_EQ(f.session(token).status, 401u);
}

// --- POST /api/v1/auth/logout ---

TEST(AuthApiTest, LoggingOutEndsTheSessionAndTheTokenStopsWorking) {
  AuthFixture f;
  f.add_user("tom", {});

  const auto token = f.token_for("tom");

  auto response = f.logout(token);
  EXPECT_EQ(response.status, 204u);
  EXPECT_TRUE(response.body.empty());

  EXPECT_EQ(f.session(token).status, 401u);
}

TEST(AuthApiTest, LoggingOutWithNoTokenIsRefusedRatherThanCongratulated) {
  AuthFixture f;

  EXPECT_EQ(f.logout("").status, 401u);
}

TEST(AuthApiTest, LoggingOutWithATokenThatNamedNothingSaysNothingAboutIt) {
  AuthFixture f;

  // A 404 here would be a way to ask whether a token is real, one guess at a time.
  EXPECT_EQ(f.logout(std::string(64, 'a')).status, 204u);
}

TEST(AuthApiTest, OneSessionLoggingOutLeavesTheOthersWorking) {
  AuthFixture f;
  f.add_user("tom", {});

  const auto first = f.token_for("tom");
  const auto second = f.token_for("tom");

  ASSERT_EQ(f.logout(first).status, 204u);

  EXPECT_EQ(f.session(first).status, 401u);
  EXPECT_EQ(f.session(second).status, 200u);
}

// --- The shape of the routes themselves ---

TEST(AuthApiTest, TheAuthRoutesTakeTheMethodsTheyAreDocumentedWith) {
  AuthFixture f;

  EXPECT_EQ(f.request(http::verb::get, "/api/v1/auth/login", "", "").status, 405u);
  EXPECT_EQ(f.request(http::verb::get, "/api/v1/auth/logout", "", "").status, 405u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/session", "admin-token", "").status, 405u);
}
