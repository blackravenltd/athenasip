//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/users_api.h"

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/signed_in_users_helper.h"
#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/auth_api.h"
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

struct UsersFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  SignedInUsers signed_in;
  std::shared_ptr<api::AuthAPI> auth;
  std::shared_ptr<api::UsersAPI> users;

  UsersFixture() {
    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);

    auto bearer = std::make_shared<api::BearerAuth>();
    router = std::make_shared<api::Router>(bearer);

    auto sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);

    auth = std::make_shared<api::AuthAPI>(logger, sessions);
    auth->register_routes(*router);

    // A password created through the API is hashed at the production count unless it is
    // told otherwise, and 600000 iterations per created user is most of a second each.
    // These are statements about the routes, not about PBKDF2.
    users = std::make_shared<api::UsersAPI>(logger, datastore, admin->executor(), sessions, kIterations);
    users->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~UsersFixture() { admin->stop(); }

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

  Response get(const std::string& target, const std::string& token = "admin-token") { return request(http::verb::get, target, token); }
  Response post(const std::string& target, const std::string& body, const std::string& token = "admin-token") {
    return request(http::verb::post, target, token, body);
  }
  Response put(const std::string& target, const std::string& body, const std::string& token = "admin-token") {
    return request(http::verb::put, target, token, body);
  }
  Response del(const std::string& target, const std::string& token = "admin-token") { return request(http::verb::delete_, target, token); }

  std::string login(const std::string& username, const std::string& password = kPassword) {
    boost::json::object body;
    body["username"] = username;
    body["password"] = password;

    auto response = request(http::verb::post, "/api/v1/auth/login", "", boost::json::serialize(body));
    EXPECT_EQ(response.status, 200u);

    return std::string(response.json().at("token").as_string());
  }
};

}  // namespace

// --- Creating ---

TEST(UsersApiTest, TheConfigurationTokenCreatesTheFirstUser) {
  UsersFixture f;

  // What a fresh node is for: there are no users, so the only credential that exists is
  // the one in the configuration file, and it has to be able to make the first one.
  auto response = f.post("/api/v1/users", R"({"username":"tom","password":"a long enough password","roles":["manage-realms"]})");

  ASSERT_EQ(response.status, 201u);

  const auto body = response.json();
  EXPECT_EQ(body.at("username").as_string(), "tom");
  EXPECT_EQ(body.at("roles").at(0).as_string(), types::roles::manage_realms);
  EXPECT_FALSE(body.at("disabled").as_bool());

  // And it can then log in, which is the whole chain this step completes.
  EXPECT_EQ(f.login("tom", "a long enough password").size(), 64u);
}

TEST(UsersApiTest, APasswordNeverComesBackOut) {
  UsersFixture f;

  auto created = f.post("/api/v1/users", R"({"username":"tom","password":"a long enough password"})");
  ASSERT_EQ(created.status, 201u);

  // Not in the creation answer, not in a read, not in the list. It is the password in the
  // only form this node holds it.
  EXPECT_EQ(created.body.find("password_hash"), std::string::npos);
  EXPECT_EQ(f.get("/api/v1/users/tom").body.find("password_hash"), std::string::npos);
  EXPECT_EQ(f.get("/api/v1/users").body.find("password_hash"), std::string::npos);
  EXPECT_EQ(created.body.find("a long enough password"), std::string::npos);
}

TEST(UsersApiTest, ATakenUsernameIsAConflictRatherThanAnOverwrite) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  auto response = f.post("/api/v1/users", R"({"username":"TOM","password":"another password"})");

  // Case-folded, so "TOM" and "tom" are the same conflict rather than two logins nobody
  // can tell apart. Agreed with the console as a 409.
  EXPECT_EQ(response.status, 409u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "conflict");

  // And the user it collided with is untouched.
  auto existing = f.store->user_get("tom");
  ASSERT_NE(existing, nullptr);
  EXPECT_TRUE(existing->has_role(types::roles::manage_realms));
}

TEST(UsersApiTest, ARoleThisBuildDoesNotKnowIsRefusedOnTheWayIn) {
  UsersFixture f;

  auto response = f.post("/api/v1/users", R"({"username":"tom","password":"a long enough password","roles":["wizard"]})");

  // The datastore carries an unknown role rather than dropping it, so that an older node
  // rewriting a user cannot silently strip a role a newer one granted. That makes this the
  // only place a typo can be caught.
  EXPECT_EQ(response.status, 400u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "unknown_role");
  EXPECT_EQ(f.store->user_get("tom"), nullptr);
}

TEST(UsersApiTest, AUserWithNoRolesIsAllowedAndStartsWithNone) {
  UsersFixture f;

  auto response = f.post("/api/v1/users", R"({"username":"tom","password":"a long enough password"})");

  ASSERT_EQ(response.status, 201u);
  EXPECT_TRUE(response.json().at("roles").as_array().empty());
}

// --- Reading ---

TEST(UsersApiTest, ListingAndReadingAUser) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});
  f.add_user("sam", {});

  // Five: these two, and the three users the fixture signs in as.
  auto list = f.get("/api/v1/users");
  ASSERT_EQ(list.status, 200u);
  EXPECT_EQ(list.json().as_array().size(), 5u);

  auto one = f.get("/api/v1/users/TOM");
  ASSERT_EQ(one.status, 200u);

  // Looked up case-insensitively, and handed back with the spelling it was created with.
  EXPECT_EQ(one.json().at("username").as_string(), "tom");
}

TEST(UsersApiTest, AUserThatDoesNotExistIsA404) {
  UsersFixture f;

  EXPECT_EQ(f.get("/api/v1/users/nobody").status, 404u);
  EXPECT_EQ(f.del("/api/v1/users/nobody").status, 404u);
  EXPECT_EQ(f.del("/api/v1/users/nobody/sessions").status, 404u);
}

// --- Changing ---

TEST(UsersApiTest, RolesGivenAsAnEmptyListTakeThemAllAway) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  auto response = f.put("/api/v1/users/tom", R"({"roles":[]})");

  // Given as empty and not given at all are different answers, which is why the field is
  // read as absent rather than as empty.
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().at("roles").as_array().empty());
}

TEST(UsersApiTest, AFieldNotGivenIsLeftAlone) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  ASSERT_EQ(f.put("/api/v1/users/tom", R"({"display_name":"Tom Cully"})").status, 200u);

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  EXPECT_EQ(user->display_name, "Tom Cully");
  EXPECT_TRUE(user->has_role(types::roles::manage_realms));
}

TEST(UsersApiTest, DisablingAUserRevokesWhatItAlreadyHeld) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});
  f.add_user("boss", {types::roles::manage_admin_users});

  const auto token = f.login("tom");
  ASSERT_EQ(f.get("/api/v1/session", token).status, 200u);

  ASSERT_EQ(f.put("/api/v1/users/tom", R"({"disabled":true})", f.login("boss")).status, 200u);

  // Immediate rather than eventual: the sessions go with the user, so a disabled user
  // does not keep working until each token happens to expire.
  EXPECT_EQ(f.get("/api/v1/session", token).status, 401u);
  EXPECT_EQ(f.store->session_get(api::Sessions::token_hash(token)), nullptr);
}

// --- Nobody locks themselves out ---

TEST(UsersApiTest, AUserCannotDisableItself) {
  UsersFixture f;
  f.add_user("boss", {types::roles::manage_admin_users});

  auto response = f.put("/api/v1/users/boss", R"({"disabled":true})", f.login("boss"));

  EXPECT_EQ(response.status, 409u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "would_lock_out");
}

TEST(UsersApiTest, AUserCannotTakeManageAdminUsersFromItself) {
  UsersFixture f;
  f.add_user("boss", {types::roles::manage_admin_users, types::roles::manage_realms});

  auto response = f.put("/api/v1/users/boss", R"({"roles":["manage-realms"]})", f.login("boss"));

  EXPECT_EQ(response.status, 409u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "would_lock_out");

  // Keeping it while changing the others is fine.
  EXPECT_EQ(f.put("/api/v1/users/boss", R"({"roles":["manage-admin-users"]})", f.login("boss")).status, 200u);
}

TEST(UsersApiTest, AUserCannotDeleteItself) {
  UsersFixture f;
  f.add_user("boss", {types::roles::manage_admin_users});

  // Deleting yourself locks you out exactly as thoroughly as disabling yourself, so the
  // rule that stops one has to stop the other or it is decorative.
  auto response = f.del("/api/v1/users/boss", f.login("boss"));

  EXPECT_EQ(response.status, 409u);
  EXPECT_NE(f.store->user_get("boss"), nullptr);
}

TEST(UsersApiTest, TheLockoutRulesAreAboutSelfAndNotAboutTheRole) {
  UsersFixture f;
  f.add_user("boss", {types::roles::manage_admin_users});
  f.add_user("other", {types::roles::manage_admin_users});

  // Somebody else holding the same role may still be disabled, demoted and deleted.
  // `athenasip --add-user` on the host is what recovers a node with no administrators left.
  const auto token = f.login("boss");
  EXPECT_EQ(f.put("/api/v1/users/other", R"({"disabled":true})", token).status, 200u);
  EXPECT_EQ(f.del("/api/v1/users/other", token).status, 204u);
}

// --- Deleting and revoking ---

TEST(UsersApiTest, DeletingAUserAnswers204AndTakesItsSessions) {
  UsersFixture f;
  f.add_user("tom", {});
  f.add_user("boss", {types::roles::manage_admin_users});

  const auto token = f.login("tom");

  auto response = f.del("/api/v1/users/tom", f.login("boss"));
  EXPECT_EQ(response.status, 204u);
  EXPECT_TRUE(response.body.empty());

  EXPECT_EQ(f.store->user_get("tom"), nullptr);
  EXPECT_EQ(f.get("/api/v1/session", token).status, 401u);
}

TEST(UsersApiTest, RevokingSessionsSignsOutEverywhereAndLeavesTheUser) {
  UsersFixture f;
  f.add_user("tom", {});

  const auto first = f.login("tom");
  const auto second = f.login("tom");

  EXPECT_EQ(f.del("/api/v1/users/tom/sessions").status, 204u);

  EXPECT_EQ(f.get("/api/v1/session", first).status, 401u);
  EXPECT_EQ(f.get("/api/v1/session", second).status, 401u);

  // The user is still there and can be logged into again, which is what makes this
  // "sign out everywhere" rather than a delete.
  EXPECT_NE(f.store->user_get("tom"), nullptr);
  EXPECT_EQ(f.login("tom").size(), 64u);
}

TEST(UsersApiTest, RevokingForAUserThatHasNeverLoggedInIs204) {
  UsersFixture f;
  f.add_user("tom", {});

  // Holding no sessions is the state the caller asked for, which is why the store treats
  // nothing to revoke as success.
  EXPECT_EQ(f.del("/api/v1/users/tom/sessions").status, 204u);
}

// --- Passwords ---

TEST(UsersApiTest, AUserChangesItsOwnPasswordWithTheOldOne) {
  UsersFixture f;
  f.add_user("tom", {});

  auto response = f.post("/api/v1/users/tom/password", R"({"old_password":"correct horse battery staple","password":"a brand new password"})", f.login("tom"));

  EXPECT_EQ(response.status, 204u);

  // The new one works and the old one does not.
  EXPECT_EQ(f.login("tom", "a brand new password").size(), 64u);

  boost::json::object body;
  body["username"] = "tom";
  body["password"] = kPassword;
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/auth/login", "", boost::json::serialize(body)).status, 401u);
}

TEST(UsersApiTest, TheWrongOldPasswordIsRefusedWithoutEndingTheSession) {
  UsersFixture f;
  f.add_user("tom", {});

  const auto token = f.login("tom");
  auto response = f.post("/api/v1/users/tom/password", R"({"old_password":"not it","password":"a brand new password"})", token);

  // 403 rather than 401, and with a code of its own. A 401 on an authenticated request
  // means the credential is no longer good, and a client that believes it signs the user
  // out; the bearer here is fine and it is a field in the body that is wrong.
  EXPECT_EQ(response.status, 403u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "wrong_password");

  // Which is the thing a client must be able to rely on: the session it presented still
  // works, so a typo does not look like being logged out.
  EXPECT_EQ(f.get("/api/v1/session", token).status, 200u);
  EXPECT_EQ(f.login("tom").size(), 64u);
}

TEST(UsersApiTest, OmittingTheOldPasswordIsRefusedForYourOwn) {
  UsersFixture f;
  f.add_user("tom", {});

  auto response = f.post("/api/v1/users/tom/password", R"({"password":"a brand new password"})", f.login("tom"));

  EXPECT_EQ(response.status, 403u);
  EXPECT_EQ(response.json().at("error").at("code").as_string(), "wrong_password");
}

TEST(UsersApiTest, TheRoleSetsAnyonesPasswordWithoutTheOldOne) {
  UsersFixture f;
  f.add_user("tom", {});
  f.add_user("boss", {types::roles::manage_admin_users});

  // Which is the case it exists for: somebody who has lost theirs cannot present it.
  EXPECT_EQ(f.post("/api/v1/users/tom/password", R"({"password":"a reset password"})", f.login("boss")).status, 204u);
  EXPECT_EQ(f.login("tom", "a reset password").size(), 64u);
}

TEST(UsersApiTest, ChangingAPasswordEndsEverySessionThatUsedTheOldOne) {
  UsersFixture f;
  f.add_user("tom", {});
  f.add_user("boss", {types::roles::manage_admin_users});

  const auto stolen = f.login("tom");
  ASSERT_EQ(f.get("/api/v1/session", stolen).status, 200u);

  ASSERT_EQ(f.post("/api/v1/users/tom/password", R"({"password":"a reset password"})", f.login("boss")).status, 204u);

  // Resetting a compromised user would be pointless if it left whoever compromised it
  // logged in.
  EXPECT_EQ(f.get("/api/v1/session", stolen).status, 401u);
}

TEST(UsersApiTest, SomebodyElsesPasswordWithoutTheRoleIsForbidden) {
  UsersFixture f;
  f.add_user("tom", {});
  f.add_user("sam", {});

  auto response = f.post("/api/v1/users/sam/password", R"({"password":"not yours to set"})", f.login("tom"));

  EXPECT_EQ(response.status, 403u);

  // Still the old password, which is the thing that matters.
  EXPECT_EQ(f.login("sam").size(), 64u);
}

TEST(UsersApiTest, AskingForSomebodyWhoDoesNotExistSaysNothingWithoutTheRole) {
  UsersFixture f;
  f.add_user("tom", {});

  // 403 before the store is asked, so this cannot be walked to find out who exists. With
  // the role it is a 404, because by then the caller is allowed to know.
  EXPECT_EQ(f.post("/api/v1/users/ghost/password", R"({"password":"x"})", f.login("tom")).status, 403u);
  EXPECT_EQ(f.post("/api/v1/users/ghost/password", R"({"password":"x"})").status, 404u);
}

// --- Who may call any of this ---

TEST(UsersApiTest, TheUsersRoutesNeedManageAdminUsers) {
  UsersFixture f;
  f.add_user("tom", {types::roles::manage_realms});

  const auto token = f.login("tom");

  EXPECT_EQ(f.get("/api/v1/users", token).status, 403u);
  EXPECT_EQ(f.post("/api/v1/users", R"({"username":"x","password":"y"})", token).status, 403u);
  EXPECT_EQ(f.get("/api/v1/users/tom", token).status, 403u);
  EXPECT_EQ(f.put("/api/v1/users/tom", R"({"display_name":"x"})", token).status, 403u);
  EXPECT_EQ(f.del("/api/v1/users/tom", token).status, 403u);
  EXPECT_EQ(f.del("/api/v1/users/tom/sessions", token).status, 403u);
}

TEST(UsersApiTest, NoneOfItIsReachableWithoutACredential) {
  UsersFixture f;
  f.add_user("tom", {});

  EXPECT_EQ(f.get("/api/v1/users", "").status, 401u);
  EXPECT_EQ(f.post("/api/v1/users/tom/password", R"({"password":"x"})", "").status, 401u);
}
