//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/https_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "../helpers/https_server_helper.h"
#include "../mocks/logger_mock.h"
#include "global_io_context.h"

using namespace athenasip;
using athenasip::push::HttpsClient;
using athenasip::push::HttpsResponse;

namespace {

HttpsClient::Options trusting_snakeoil(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  HttpsClient::Options options;
  options.ca_file = TestHttpsServer::ca_file();
  options.timeout = timeout;
  return options;
}

plugins::Result<HttpsResponse> post(HttpsClient& client, const std::string& url, push::HttpsHeaders headers = {}, std::string body = {}) {
  std::promise<plugins::Result<HttpsResponse>> answered;
  auto future = answered.get_future();

  client.post(detail::get_global_io_context().get_executor(), url, std::move(headers), std::move(body),
              [&answered](plugins::Result<HttpsResponse> result) { answered.set_value(std::move(result)); });

  if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready) return plugins::Result<HttpsResponse>::failure("test: no answer");
  return future.get();
}

}  // namespace

TEST(HttpsClientTest, PostsTheRequestAndReturnsTheResponse) {
  TestHttpsServer server([](const TestHttpsServer::Request&) {
    TestHttpsServer::Reply reply;
    reply.status = 201;
    reply.body = "created";
    reply.headers = {{"Location", "https://push.example/m/1"}};
    return reply;
  });

  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());
  ASSERT_TRUE(client.error().empty()) << client.error();

  auto result = post(client, server.url("/push/abc?x=1"), {{"TTL", "60"}, {"Content-Type", "text/plain"}}, "hello");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.status, 201u);
  EXPECT_EQ(result.value.body, "created");
  EXPECT_EQ(result.value.header("location"), "https://push.example/m/1");

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].method, "POST");
  EXPECT_EQ(requests[0].target, "/push/abc?x=1");
  // RFC 9110 7.2: Host carries the port when it is not the scheme's default.
  EXPECT_EQ(requests[0].header("Host"), "127.0.0.1:" + std::to_string(server.port()));
  EXPECT_EQ(requests[0].header("TTL"), "60");
  EXPECT_EQ(requests[0].header("Content-Type"), "text/plain");
  EXPECT_EQ(requests[0].header("Content-Length"), "5");
  EXPECT_EQ(requests[0].body, "hello");
}

TEST(HttpsClientTest, AnEmptyBodyIsSentWithAZeroContentLength) {
  TestHttpsServer server([](const TestHttpsServer::Request&) { return TestHttpsServer::Reply{}; });
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto result = post(client, server.url("/"));
  ASSERT_TRUE(result.ok) << result.error;

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header("Content-Length"), "0");
  EXPECT_TRUE(requests[0].body.empty());
}

TEST(HttpsClientTest, AnyStatusIsAResponseNotAnError) {
  TestHttpsServer server([](const TestHttpsServer::Request&) {
    TestHttpsServer::Reply reply;
    reply.status = 410;
    return reply;
  });
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto result = post(client, server.url("/gone"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.status, 410u);
}

// RFC 6066 3: the client names the host it wants, and never a literal address.
TEST(HttpsClientTest, SendsTheServerNameForAHostNameOnly) {
  TestHttpsServer server([](const TestHttpsServer::Request&) { return TestHttpsServer::Reply{}; });
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());

  ASSERT_TRUE(post(client, server.url("/named", "localhost")).ok);
  ASSERT_TRUE(post(client, server.url("/literal", "127.0.0.1")).ok);

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0].server_name, "localhost");
  EXPECT_EQ(requests[0].header("Host"), "localhost:" + std::to_string(server.port()));
  EXPECT_TRUE(requests[1].server_name.empty());
}

// RFC 2818 3.1: the certificate is verified, so one from a CA nobody trusts is refused
// before anything is sent.
TEST(HttpsClientTest, RefusesACertificateFromAnUntrustedAuthority) {
  TestHttpsServer server([](const TestHttpsServer::Request&) { return TestHttpsServer::Reply{}; });

  HttpsClient::Options options;
  options.timeout = std::chrono::seconds(5);
  HttpsClient client(std::make_shared<MockLogger>(), options);

  auto result = post(client, server.url("/"));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("TLS handshake"), std::string::npos) << result.error;
  EXPECT_TRUE(server.requests().empty());
}

TEST(HttpsClientTest, GivesUpAtTheDeadline) {
  TestHttpsServer server([](const TestHttpsServer::Request&) {
    TestHttpsServer::Reply reply;
    reply.silent = true;
    return reply;
  });
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil(std::chrono::milliseconds(300)));

  const auto started = std::chrono::steady_clock::now();
  auto result = post(client, server.url("/slow"));
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("timed out"), std::string::npos) << result.error;
  EXPECT_LT(took, std::chrono::seconds(5));
}

TEST(HttpsClientTest, RefusesAnythingButHttps) {
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());

  EXPECT_FALSE(post(client, "http://127.0.0.1/").ok);
  EXPECT_FALSE(post(client, "not a url").ok);
}

TEST(HttpsClientTest, ReportsACaFileThatWillNotLoad) {
  HttpsClient::Options options;
  options.ca_file = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/nosuch.crt";
  HttpsClient client(std::make_shared<MockLogger>(), options);

  EXPECT_FALSE(client.error().empty());
  EXPECT_FALSE(post(client, "https://127.0.0.1/").ok);
}

TEST(HttpsClientTest, AnswersOnTheCallersExecutor) {
  TestHttpsServer server([](const TestHttpsServer::Request&) { return TestHttpsServer::Reply{}; });
  HttpsClient client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto strand = boost::asio::make_strand(detail::get_global_io_context());
  std::promise<bool> on_strand;
  auto future = on_strand.get_future();

  client.post(strand, server.url("/"), {}, {},
              [&on_strand, strand](plugins::Result<HttpsResponse> result) { on_strand.set_value(result.ok && strand.running_in_this_thread()); });

  ASSERT_EQ(future.wait_for(std::chrono::seconds(15)), std::future_status::ready);
  EXPECT_TRUE(future.get());
}
