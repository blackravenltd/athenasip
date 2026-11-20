//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/security_api.h"

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/bus_heard_helper.h"
#include "../helpers/core_fixture_helper.h"
#include "../helpers/signed_in_users_helper.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "auth_guard.h"
#include "events/topics.h"

using namespace athenasip;
using namespace std::chrono_literals;

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

struct SecurityFixture : CoreFixture {
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  SignedInUsers signed_in;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::SecurityAPI> security;

  SecurityFixture() {
    config->sip_auth_failures_source = {1, 1};
    config->sip_auth_failures_aor = {1, 1};
    core->auth_limits_reload();

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);
    router = std::make_shared<api::Router>(bearer);
    security = std::make_shared<api::SecurityAPI>(logger, core, admin->executor());
    security->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~SecurityFixture() { admin->stop(); }

  Response request(http::verb method, const std::string& target, const std::string& token = "admin-token") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);
    socket.close(ec);
    return Response{response.result_int(), response.body()};
  }

  // Two wrong answers from one address for one AOR: the address banned, the AOR closed.
  void guess(const std::string& source, const std::string& aor) {
    core->auth_guard().failed(source, aor);
    core->auth_guard().failed(source, aor);
  }
};

}  // namespace

// The bans in force, with the node that made each and until when, for whoever may read the cluster's status.
TEST(SecurityApiTest, TheBansInForceAreListed) {
  SecurityFixture f;
  f.guess("198.51.100.7", "sip:alice@example.com");

  auto response = f.request(http::verb::get, "/api/v1/security/bans", "client-token");
  ASSERT_EQ(response.status, 200u) << response.body;
  const auto bans = response.json().as_array();
  ASSERT_EQ(bans.size(), 1u);
  EXPECT_EQ(bans[0].at("source").as_string(), "198.51.100.7");
  EXPECT_EQ(bans[0].at("node").as_string(), "test-node");
  EXPECT_GT(bans[0].at("seconds_left").as_int64(), 0);
  EXPECT_FALSE(bans[0].at("until").as_string().empty());

  auto closed = f.request(http::verb::get, "/api/v1/security/closed-aors", "client-token");
  ASSERT_EQ(closed.status, 200u) << closed.body;
  ASSERT_EQ(closed.json().as_array().size(), 1u);
  EXPECT_EQ(closed.json().as_array()[0].at("aor").as_string(), "sip:alice@example.com");

  EXPECT_EQ(f.request(http::verb::get, "/api/v1/security/bans", "scopeless-token").status, 403u);
}

// A lift takes manage-cluster, ends the ban here, forgets the failures, and is told to every other node.
TEST(SecurityApiTest, ALiftEndsTheBanAndIsToldToTheCluster) {
  SecurityFixture f;
  f.guess("198.51.100.7", "sip:alice@example.com");

  auto heard = std::make_shared<BusHeard>();
  f.bus->subscribe(events::topics::ban_lifted(), BusHeard::into(heard));

  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/security/bans/198.51.100.7", "client-token").status, 403u) << "reading is not lifting";

  auto lifted = f.request(http::verb::delete_, "/api/v1/security/bans/198.51.100.7");
  ASSERT_EQ(lifted.status, 200u) << lifted.body;
  EXPECT_FALSE(f.core->auth_guard().banned("198.51.100.7"));
  EXPECT_FALSE(f.core->auth_guard().failed("198.51.100.7", "sip:bob@example.com").banned) << "its count started again";
  const auto lifts = heard->wait_for(1);
  ASSERT_EQ(lifts.size(), 1u);
  EXPECT_NE(lifts[0].find("198.51.100.7"), std::string::npos);

  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/security/bans/203.0.113.1").status, 404u);
}

// An AOR is reopened by its name, however it is spelt.
TEST(SecurityApiTest, AClosedAorIsReopened) {
  SecurityFixture f;
  f.guess("198.51.100.7", "sip:alice@example.com");

  auto reopened = f.request(http::verb::delete_, "/api/v1/security/closed-aors/sip%3Aalice%40EXAMPLE.com");
  ASSERT_EQ(reopened.status, 200u) << reopened.body;
  EXPECT_FALSE(f.core->auth_guard().aor_closed("203.0.113.9", "sip:alice@example.com"));
}

// Another node's ban, heard on the bus, is held here and listed as that node's; its lift, heard too, ends it.
TEST(SecurityApiTest, AnotherNodesBanIsHeldAndItsLiftEndsIt) {
  SecurityFixture f;
  f.on_strand([&]() { f.core->auth_listen(); });
  std::this_thread::sleep_for(50ms);

  ASSERT_TRUE(f.bus->publish(events::topics::source_banned(), R"({"source":"203.0.113.77","seconds":600,"node":"node-b","at":"x"})"));
  for (int i = 0; i < 100 && !f.core->auth_guard().banned("203.0.113.77"); ++i) std::this_thread::sleep_for(5ms);
  ASSERT_TRUE(f.core->auth_guard().banned("203.0.113.77"));
  const auto bans = f.request(http::verb::get, "/api/v1/security/bans").json().as_array();
  ASSERT_EQ(bans.size(), 1u);
  EXPECT_EQ(bans[0].at("node").as_string(), "node-b");

  ASSERT_TRUE(f.bus->publish(events::topics::ban_lifted(), R"({"source":"203.0.113.77","node":"node-b"})"));
  for (int i = 0; i < 100 && f.core->auth_guard().banned("203.0.113.77"); ++i) std::this_thread::sleep_for(5ms);
  EXPECT_FALSE(f.core->auth_guard().banned("203.0.113.77"));
}
