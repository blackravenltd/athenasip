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
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/signed_in_users_helper.h"
#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/auth_api.h"
#include "api/rate_limiter.h"
#include "api/router.h"
#include "api/sessions.h"
#include "datastores/memory_datastore.h"
#include "types/password.h"
#include "types/url.h"
#include "types/user.h"

using namespace athenasip;
using namespace std::chrono_literals;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

constexpr const char* kPassword = "correct horse battery staple";

struct Response {
  unsigned status = 0;
  std::string retry_after;
};

// Every route is rate limited: open routes hard, by source address; signed-in callers generously, by session.
// The limits under test are the shipped ones; the clock is the test's.
struct LimitFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Throttle> throttle = std::make_shared<api::Throttle>();
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::AuthAPI> auth;
  SignedInUsers signed_in;
  api::RateLimiter::Clock::time_point now = api::RateLimiter::Clock::now();

  LimitFixture() {
    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    throttle->clock_set([this] { return now; });

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);

    auto bearer = std::make_shared<api::BearerAuth>();
    router = std::make_shared<api::Router>(bearer, throttle);
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);

    auth = std::make_shared<api::AuthAPI>(logger, sessions);
    auth->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);

    auto user = std::make_shared<types::User>();
    user->username = "tom";
    user->roles = types::roles::all();
    user->password_hash = types::Password::hash(kPassword, 1000);
    user->created_at = std::time(nullptr);
    EXPECT_TRUE(store->user_create(user));
  }

  ~LimitFixture() { admin->stop(); }

  Response request(http::verb method, const std::string& target, const std::string& bearer = "", const std::string& body = "") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!bearer.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(bearer));
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

    return Response{response.result_int(), std::string(response[http::field::retry_after])};
  }

  Response login(const std::string& username, const std::string& password = kPassword) {
    boost::json::object body;
    body["username"] = username;
    body["password"] = password;
    return request(http::verb::post, "/api/v1/auth/login", "", boost::json::serialize(body));
  }

  Response session(const std::string& bearer) { return request(http::verb::get, "/api/v1/session", bearer); }
};

}  // namespace

// An open route answers a burst and then 429, with how long to wait.
TEST(RateLimitTest, AnOpenRouteIsLimitedBySourceAddress) {
  LimitFixture f;

  for (int i = 0; i < 30; ++i) ASSERT_EQ(f.request(http::verb::post, "/api/v1/auth/logout", "names-nothing").status, 204u) << "request " << i;

  const auto refused = f.request(http::verb::post, "/api/v1/auth/logout", "names-nothing");
  EXPECT_EQ(refused.status, 429u);
  EXPECT_EQ(refused.retry_after, "2");

  // Waiting as long as it said is enough.
  f.now += 2s;
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/auth/logout", "names-nothing").status, 204u);
}

// A token that resolves to nobody counts against its source address, like an open route.
TEST(RateLimitTest, ACredentialThatDoesNotResolveIsLimitedBySourceAddress) {
  LimitFixture f;

  for (int i = 0; i < 30; ++i) ASSERT_EQ(f.session("not-a-token-" + std::to_string(i)).status, 401u);

  EXPECT_EQ(f.session("not-a-token").status, 429u);

  // A signed-in caller on the same address is not limited by it.
  EXPECT_EQ(f.session("admin-token").status, 200u);
}

// A scan for endpoints is limited like an open route.
TEST(RateLimitTest, AnEndpointThatIsNotThereIsLimitedToo) {
  LimitFixture f;

  for (int i = 0; i < 30; ++i) ASSERT_EQ(f.request(http::verb::get, "/api/v1/nothing-" + std::to_string(i)).status, 404u);

  EXPECT_EQ(f.request(http::verb::get, "/api/v1/nothing").status, 429u);
}

// Guesses at one user's password are limited after five, whoever is guessing.
TEST(RateLimitTest, LoginIsLimitedPerUsername) {
  LimitFixture f;

  for (int i = 0; i < 5; ++i) ASSERT_EQ(f.login("tom", "wrong").status, 401u) << "attempt " << i;

  const auto refused = f.login("tom");
  EXPECT_EQ(refused.status, 429u) << "even with the right password: the limit is on attempts, not on failures";
  EXPECT_EQ(refused.retry_after, "60");

  // Another username is still open from this address.
  EXPECT_EQ(f.login("somebody", "wrong").status, 401u);

  f.now += 60s;
  EXPECT_EQ(f.login("tom").status, 200u);
}

// Guesses spread across usernames are limited per address.
TEST(RateLimitTest, LoginIsLimitedPerSourceAddress) {
  LimitFixture f;

  for (int i = 0; i < 10; ++i) ASSERT_EQ(f.login("user-" + std::to_string(i), "wrong").status, 401u) << "attempt " << i;

  EXPECT_EQ(f.login("user-10", "wrong").status, 429u);
}

// A console polling every two seconds, three requests at a time, is never limited.
TEST(RateLimitTest, ASignedInConsolePollingEveryTwoSecondsIsNeverLimited) {
  LimitFixture f;

  for (int poll = 0; poll < 60; ++poll) {
    for (int i = 0; i < 3; ++i) ASSERT_EQ(f.session("admin-token").status, 200u) << "poll " << poll;
    f.now += 2s;
  }
}

// Each session has its own limit.
TEST(RateLimitTest, ASessionIsLimitedOnItsOwn) {
  LimitFixture f;

  for (int i = 0; i < 60; ++i) ASSERT_EQ(f.session("admin-token").status, 200u) << "request " << i;

  const auto refused = f.session("admin-token");
  EXPECT_EQ(refused.status, 429u);
  EXPECT_EQ(refused.retry_after, "1");

  EXPECT_EQ(f.session("client-token").status, 200u);
}
