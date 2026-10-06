//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <string>

#include "../helpers/proxy_fixture_helper.h"
#include "../helpers/signed_in_users_helper.h"
#include "api/admin_api.h"
#include "api/calls_api.h"
#include "api/router.h"

using namespace athenasip;

namespace {

namespace http = boost::beast::http;

// DELETE /api/v1/calls/{call} against a node carrying a real call between two channels.
struct HangUpApiFixture : ProxyFixture {
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::CallsAPI> calls;
  SignedInUsers signed_in;

  HangUpApiFixture() {
    bind_bob();

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

  ~HangUpApiFixture() { admin->stop(); }

  unsigned hang_up(const std::string& call_id, const std::string& token = "admin-token") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{http::verb::delete_, "/api/v1/calls/" + call_id, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
    request.prepare_payload();
    http::write(socket, request);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);
    socket.close(ec);

    settle();
    return response.result_int();
  }
};

}  // namespace

// The administrator ends a call: each end is sent a BYE, and the answer says it is on its way.
TEST(CallsHangUpTest, HangingUpSendsEachEndABye) {
  HangUpApiFixture f;
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.settle();

  EXPECT_EQ(f.hang_up("call-proxy"), 202u);

  EXPECT_EQ(ProxyFixture::requests_with(f.caller_connection, "BYE").size(), 1u);
  EXPECT_EQ(ProxyFixture::requests_with(f.callee_connection, "BYE").size(), 1u);
}

TEST(CallsHangUpTest, ACallThatIsNotLiveIsNotFound) {
  HangUpApiFixture f;

  EXPECT_EQ(f.hang_up("nobody%40example.com"), 404u);
}

// A call still ringing has no dialog for a BYE (RFC 3261 15); the caller ends it with a CANCEL.
TEST(CallsHangUpTest, ACallNotYetAnsweredIsAConflict) {
  HangUpApiFixture f;
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  f.settle();

  EXPECT_EQ(f.hang_up("call-proxy"), 409u);
  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "BYE").empty());
}

// Ending calls changes the cluster's state, so it takes manage-cluster; reading calls is not enough.
TEST(CallsHangUpTest, HangingUpTakesTheManageClusterRole) {
  HangUpApiFixture f;
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.settle();

  EXPECT_EQ(f.hang_up("call-proxy", ""), 401u);
  EXPECT_EQ(f.hang_up("call-proxy", "client-token"), 403u);
  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "BYE").empty());
}
