//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/http2_client.h"

#include <gtest/gtest.h>

#include <boost/asio/strand.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/http2_server_helper.h"
#include "../mocks/logger_mock.h"
#include "global_io_context.h"

using namespace athenasip;
using athenasip::push::Http2Client;
using athenasip::push::HttpsResponse;

namespace {

Http2Client::Options trusting_snakeoil(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  Http2Client::Options options;
  options.ca_file = TestHttp2Server::ca_file();
  options.timeout = timeout;
  return options;
}

using Answer = std::future<plugins::Result<HttpsResponse>>;

Answer post_async(Http2Client& client, const std::string& url, push::HttpsHeaders headers = {}, std::string body = {}) {
  auto answered = std::make_shared<std::promise<plugins::Result<HttpsResponse>>>();
  auto future = answered->get_future();

  client.post(detail::get_global_io_context().get_executor(), url, std::move(headers), std::move(body),
              [answered](plugins::Result<HttpsResponse> result) { answered->set_value(std::move(result)); });
  return future;
}

plugins::Result<HttpsResponse> wait(Answer& future) {
  if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready) return plugins::Result<HttpsResponse>::failure("test: no answer");
  return future.get();
}

plugins::Result<HttpsResponse> post(Http2Client& client, const std::string& url, push::HttpsHeaders headers = {}, std::string body = {}) {
  auto future = post_async(client, url, std::move(headers), std::move(body));
  return wait(future);
}

TestHttp2Server::Reply ok() { return TestHttp2Server::Reply{}; }

}  // namespace

// RFC 9113 8.3.1: a request carries :method, :scheme, :authority and :path, and its other
// fields in lowercase (8.2.1).
TEST(Http2ClientTest, PostsTheRequestAndReturnsTheResponse) {
  TestHttp2Server server([](const TestHttp2Server::Request&) {
    TestHttp2Server::Reply reply;
    reply.status = 201;
    reply.body = "{\"created\":true}";
    reply.headers = {{"apns-id", "EC1BF194-B3B2-4A3E-8B8A-2E5D7A5D0C1F"}};
    return reply;
  });

  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());
  ASSERT_TRUE(client.error().empty()) << client.error();

  auto result = post(client, server.url("/3/device/00fc13adff78512?x=1"), {{"Apns-Topic", "com.example.app"}, {"apns-priority", "10"}}, "{\"a\":1}");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.status, 201u);
  EXPECT_EQ(result.value.body, "{\"created\":true}");
  EXPECT_EQ(result.value.header("apns-id"), "EC1BF194-B3B2-4A3E-8B8A-2E5D7A5D0C1F");
  EXPECT_TRUE(result.value.header(":status").empty());

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].method, "POST");
  EXPECT_EQ(requests[0].scheme, "https");
  EXPECT_EQ(requests[0].authority, "127.0.0.1:" + std::to_string(server.port()));
  EXPECT_EQ(requests[0].path, "/3/device/00fc13adff78512?x=1");
  EXPECT_EQ(requests[0].header_count("apns-topic"), 1u);
  EXPECT_EQ(requests[0].header("apns-topic"), "com.example.app");
  EXPECT_EQ(requests[0].header("apns-priority"), "10");
  EXPECT_EQ(requests[0].header("content-length"), "7");
  EXPECT_EQ(requests[0].body, "{\"a\":1}");
}

TEST(Http2ClientTest, AnyStatusIsAResponseNotAnError) {
  TestHttp2Server server([](const TestHttp2Server::Request&) {
    TestHttp2Server::Reply reply;
    reply.status = 410;
    reply.body = "{\"reason\":\"Unregistered\"}";
    return reply;
  });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto result = post(client, server.url("/gone"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.status, 410u);
  EXPECT_EQ(result.value.body, "{\"reason\":\"Unregistered\"}");
}

// RFC 6066 3: SNI names a host, never a literal address.
TEST(Http2ClientTest, SendsTheServerNameForAHostNameOnly) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  ASSERT_TRUE(post(client, server.url("/named", "localhost")).ok);
  ASSERT_TRUE(post(client, server.url("/literal")).ok);

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0].server_name, "localhost");
  EXPECT_EQ(requests[0].authority, "localhost:" + std::to_string(server.port()));
  EXPECT_TRUE(requests[1].server_name.empty());
}

// APNs asks that one connection be kept and used for many notifications.
TEST(Http2ClientTest, KeepsOneConnectionForEveryRequestToAnOrigin) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  ASSERT_TRUE(post(client, server.url("/one")).ok);
  ASSERT_TRUE(post(client, server.url("/two")).ok);

  // RFC 9113 5: concurrent requests are streams of the same connection.
  std::vector<Answer> answers;
  for (int i = 0; i < 8; ++i) answers.push_back(post_async(client, server.url("/many/" + std::to_string(i))));
  for (auto& answer : answers) {
    auto result = wait(answer);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.value.status, 200u);
  }

  EXPECT_EQ(server.connections(), 1);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 10u);
  for (const auto& request : requests) EXPECT_EQ(request.connection, 1);
}

TEST(Http2ClientTest, ReconnectsWhenTheConnectionHasGone) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  ASSERT_TRUE(post(client, server.url("/before")).ok);
  server.drop_connections();

  // Whether or not the client has noticed yet, the request is answered on a new connection.
  auto result = post(client, server.url("/after"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.status, 200u);

  EXPECT_EQ(server.connections(), 2);
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].path, "/after");
  EXPECT_EQ(requests[1].connection, 2);
}

// RFC 9113 3.2: HTTP/2 over TLS is negotiated by ALPN; a server that does not choose h2
// does not speak it.
TEST(Http2ClientTest, RefusesAServerThatDoesNotAgreeToHttp2) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); }, false);
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto result = post(client, server.url("/h1"));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("HTTP/2"), std::string::npos) << result.error;
  EXPECT_TRUE(server.requests().empty());
}

TEST(Http2ClientTest, RefusesACertificateFromAnUntrustedAuthority) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); });

  // Only the system's roots, which did not sign the snakeoil certificate.
  Http2Client client(std::make_shared<MockLogger>(), Http2Client::Options{});

  auto result = post(client, server.url("/untrusted"));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(server.requests().empty());
}

TEST(Http2ClientTest, GivesUpAtTheDeadline) {
  TestHttp2Server server([](const TestHttp2Server::Request& request) {
    TestHttp2Server::Reply reply;
    reply.silent = request.path == "/silent";
    return reply;
  });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil(std::chrono::milliseconds(300)));

  const auto started = std::chrono::steady_clock::now();
  auto result = post(client, server.url("/silent"));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("timed out"), std::string::npos) << result.error;
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));

  // The stream was cancelled, not the connection: the next request still goes on it.
  ASSERT_TRUE(post(client, server.url("/after")).ok);
  EXPECT_EQ(server.connections(), 1);
}

TEST(Http2ClientTest, RefusesAnythingButHttps) {
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());
  EXPECT_FALSE(post(client, "http://127.0.0.1:1/x").ok);
  EXPECT_FALSE(post(client, "not a url").ok);
}

TEST(Http2ClientTest, ReportsACaFileThatWillNotLoad) {
  Http2Client::Options options;
  options.ca_file = "/nonexistent/ca.pem";
  Http2Client client(std::make_shared<MockLogger>(), options);

  EXPECT_FALSE(client.error().empty());
  EXPECT_FALSE(post(client, "https://127.0.0.1:1/x").ok);
}

TEST(Http2ClientTest, AnswersOnTheCallersExecutor) {
  TestHttp2Server server([](const TestHttp2Server::Request&) { return ok(); });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto strand = boost::asio::make_strand(detail::get_global_io_context());
  std::promise<bool> answered;
  auto future = answered.get_future();

  client.post(strand, server.url("/strand"), {}, {},
              [&answered, strand](plugins::Result<HttpsResponse> result) { answered.set_value(result.ok && strand.running_in_this_thread()); });

  ASSERT_EQ(future.wait_for(std::chrono::seconds(15)), std::future_status::ready);
  EXPECT_TRUE(future.get());
}

TEST(Http2ClientTest, ClosingFailsWhatIsStillWaiting) {
  TestHttp2Server server([](const TestHttp2Server::Request&) {
    TestHttp2Server::Reply reply;
    reply.silent = true;
    return reply;
  });
  Http2Client client(std::make_shared<MockLogger>(), trusting_snakeoil());

  auto answer = post_async(client, server.url("/held"));
  for (int i = 0; i < 500 && server.requests().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(server.requests().size(), 1u);

  client.close();
  auto result = wait(answer);
  EXPECT_FALSE(result.ok);
}
