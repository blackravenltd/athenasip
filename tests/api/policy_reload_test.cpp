//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../helpers/core_fixture_helper.h"
#include "../helpers/signed_in_users_helper.h"
#include "api/admin_api.h"
#include "api/calls_api.h"
#include "api/router.h"
#include "policy/lua_policy.h"

using namespace athenasip;

namespace {

namespace http = boost::beast::http;

// A node running lua:// from a directory the test writes, with the admin API in front of it.
struct ReloadFixture : CoreFixture {
  std::filesystem::path scripts;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::CallsAPI> calls;
  SignedInUsers signed_in;

  ReloadFixture() {
    scripts = std::filesystem::temp_directory_path() / ("athenasip-reload-" + std::to_string(::getpid()));
    std::filesystem::create_directories(scripts);
    write("function route(request) return athenasip.route.reply(486, 'first') end");

    auto policy = std::make_shared<policy::LuaPolicy>(logger, nullptr);
    YAML::Node own;
    own["path"].push_back(scripts.string());
    EXPECT_TRUE(policy->configure(own, *config)) << policy->error();
    on_strand([this, policy]() { core->policy_register(policy); });

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);
    router = std::make_shared<api::Router>(bearer);
    calls = std::make_shared<api::CallsAPI>(logger, core, admin->executor());
    calls->register_routes(*router);
    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~ReloadFixture() {
    admin->stop();
    std::error_code ignored;
    std::filesystem::remove_all(scripts, ignored);
  }

  void write(const std::string& main) const { std::ofstream(scripts / "main.lua") << main; }

  std::pair<unsigned, std::string> reload(const std::string& token = "admin-token") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{http::verb::post, "/api/v1/policy/reload", 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
    request.prepare_payload();
    http::write(socket, request);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);
    socket.close(ec);
    return {response.result_int(), response.body()};
  }

  std::string fingerprint() {
    return on_strand([this]() { return core->policy()->fingerprint(); });
  }
};

}  // namespace

// New scripts take over, and the answer says which are now in force.
TEST(PolicyReloadTest, AReloadLoadsTheScriptsAgain) {
  ReloadFixture f;
  const auto before = f.fingerprint();
  ASSERT_FALSE(before.empty());

  f.write("function route(request) return athenasip.route.reply(486, 'second') end");
  const auto [code, body] = f.reload();

  EXPECT_EQ(code, 200u) << body;
  const auto after = f.fingerprint();
  EXPECT_NE(after, before);
  EXPECT_EQ(boost::json::parse(body).as_object().at("fingerprint").as_string(), after);
}

// Scripts that do not load are refused with their line, and the ones in force stay.
TEST(PolicyReloadTest, BrokenScriptsAreRefusedAndTheRunningOnesStay) {
  ReloadFixture f;
  const auto before = f.fingerprint();

  f.write("function route(request\n");
  const auto [code, body] = f.reload();

  EXPECT_EQ(code, 422u);
  EXPECT_NE(body.find("main.lua:2"), std::string::npos) << body;
  EXPECT_EQ(f.fingerprint(), before);
}

// Changing what decides every call changes the cluster: viewing it is not enough.
TEST(PolicyReloadTest, ReloadingTakesManageCluster) {
  ReloadFixture f;
  EXPECT_EQ(f.reload("client-token").first, 403u);
  EXPECT_EQ(f.reload("").first, 401u);
}

// The node's status says which scripts it runs, so a console can see nodes that disagree.
TEST(PolicyReloadTest, TheStatusCarriesTheScriptsFingerprint) {
  ReloadFixture f;
  const auto status = boost::json::parse(f.on_strand([&f]() { return f.core->node_status_json("ok"); })).as_object();

  const auto& policy = status.at("policy").as_object();
  EXPECT_EQ(policy.at("driver").as_string(), "lua 1.0.0");
  EXPECT_EQ(policy.at("fingerprint").as_string(), f.fingerprint());
}
