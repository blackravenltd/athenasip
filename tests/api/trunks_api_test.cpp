//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/trunks_api.h"

#include <gtest/gtest.h>

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
#include "api/sessions.h"
#include "datastores/memory_datastore.h"
#include "types/url.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

struct Response {
  unsigned status = 0;
  std::string body;

  boost::json::object json() const { return boost::json::parse(body).as_object(); }
};

struct TrunksFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::TrunksAPI> trunks;
  SignedInUsers signed_in;

  TrunksFixture() {
    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    router = std::make_shared<api::Router>(bearer);
    auto sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);

    trunks = std::make_shared<api::TrunksAPI>(logger, datastore, admin->executor());
    trunks->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~TrunksFixture() { admin->stop(); }

  Response request(http::verb method, const std::string& target, const std::string& body = "", const std::string& token = "admin-token") {
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

  static std::string acme() {
    return R"({"name": "acme", "uri": "sip:sip.acme.example;transport=tls", "username": "4420", "password": "s3cret",
               "register": {"enabled": true, "expires": 300}, "inbound_addresses": ["203.0.113.0/24"],
               "attributes": {"prefixes": ["+44"]}})";
  }
};

}  // namespace

// A trunk goes in with its password and comes back out without it.
TEST(TrunksApiTest, ATrunkIsCreatedAndItsPasswordNeverComesBack) {
  TrunksFixture f;

  const auto created = f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme());
  ASSERT_EQ(created.status, 201u) << created.body;
  EXPECT_EQ(created.body.find("s3cret"), std::string::npos);
  EXPECT_TRUE(created.json().at("password_set").as_bool());

  const auto read = f.request(http::verb::get, "/api/v1/trunks/ACME");
  ASSERT_EQ(read.status, 200u);
  EXPECT_EQ(read.body.find("s3cret"), std::string::npos);
  EXPECT_EQ(read.json().at("attributes").as_object().at("prefixes").as_array().at(0).as_string(), "+44");

  EXPECT_EQ(f.store->trunk_get("acme")->password, "s3cret") << "stored as given, for answering the carrier's challenge";
  EXPECT_EQ(f.request(http::verb::get, "/api/v1/trunks").body.find("s3cret"), std::string::npos);
}

TEST(TrunksApiTest, ATakenNameIsAConflict) {
  TrunksFixture f;
  ASSERT_EQ(f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme()).status, 201u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme()).status, 409u);
}

// What the node would act on is checked on the way in.
TEST(TrunksApiTest, WhatCannotBeUsedIsRefused) {
  TrunksFixture f;
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", R"({"name": "acme uk", "uri": "sip:x.example"})").status, 400u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", R"({"name": "acme"})").status, 400u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", R"({"name": "acme", "uri": "http://x.example"})").status, 400u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", R"({"name": "acme", "uri": "sip:x.example", "inbound_addresses": ["10.0.0.0/40"]})").status, 400u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", R"({"name": "acme", "uri": "sip:x.example", "register": {"enabled": true}})").status, 400u)
      << "a trunk that registers needs someone to register as";
}

// An update changes what it names and nothing else; a password left out is kept.
TEST(TrunksApiTest, AnUpdateChangesOnlyWhatItGives) {
  TrunksFixture f;
  ASSERT_EQ(f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme()).status, 201u);

  const auto updated = f.request(http::verb::put, "/api/v1/trunks/acme", R"({"inbound_addresses": ["198.51.100.0/24"]})");
  ASSERT_EQ(updated.status, 200u) << updated.body;

  const auto stored = f.store->trunk_get("acme");
  EXPECT_EQ(stored->inbound_addresses, std::vector<std::string>{"198.51.100.0/24"});
  EXPECT_EQ(stored->password, "s3cret");
  EXPECT_EQ(stored->uri, "sip:sip.acme.example;transport=tls");

  EXPECT_EQ(f.request(http::verb::put, "/api/v1/trunks/acme", R"({"name": "other"})").status, 400u);
  EXPECT_EQ(f.request(http::verb::put, "/api/v1/trunks/nobody", R"({})").status, 404u);
}

TEST(TrunksApiTest, ADeletedTrunkIsGone) {
  TrunksFixture f;
  ASSERT_EQ(f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme()).status, 201u);
  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/trunks/acme").status, 204u);
  EXPECT_EQ(f.request(http::verb::get, "/api/v1/trunks/acme").status, 404u);
  EXPECT_EQ(f.request(http::verb::delete_, "/api/v1/trunks/acme").status, 404u);
}

// A trunk's credentials are the carrier's: only manage-trunks reads or writes them.
TEST(TrunksApiTest, TrunksTakeManageTrunks) {
  TrunksFixture f;
  EXPECT_EQ(f.request(http::verb::get, "/api/v1/trunks", "", "client-token").status, 403u);
  EXPECT_EQ(f.request(http::verb::post, "/api/v1/trunks", TrunksFixture::acme(), "client-token").status, 403u);
  EXPECT_EQ(f.request(http::verb::get, "/api/v1/trunks", "", "").status, 401u);
}
