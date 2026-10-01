//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/calls_api.h"

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <string>

#include "../helpers/core_fixture_helper.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "call.h"
#include "media/media_engine.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

// An engine that answers with what a test gives it.
class StubEngine : public media::MediaEngine {
 public:
  std::string name() const override { return "stub"; }
  std::string version() const override { return "0.0.1"; }

  void connect(plugins::Executor on, plugins::StatusHandler handler) override { _complete(std::move(on), std::move(handler), plugins::Status::success()); }
  void close() override {}
  bool is_connected() const override { return true; }

  media::Capabilities capabilities() const override {
    media::Capabilities capabilities;
    capabilities.bridge = true;
    capabilities.record = true;
    return capabilities;
  }

  void offer(plugins::Executor on, std::shared_ptr<Call>, std::string sdp, media::Flags, MediaHandler handler) override {
    _complete(std::move(on), std::move(handler), media::Result::success(std::move(sdp)));
  }
  void answer(plugins::Executor on, std::shared_ptr<Call>, std::string sdp, media::Flags, MediaHandler handler) override {
    _complete(std::move(on), std::move(handler), media::Result::success(std::move(sdp)));
  }
  void release(plugins::Executor on, std::shared_ptr<Call>, plugins::StatusHandler handler) override {
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void query(plugins::Executor on, std::shared_ptr<Call>, plugins::Handler<std::string> handler) override {
    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(document));
  }

  std::optional<std::uint64_t> packets_relayed() const override { return 123; }

  std::string document =
      R"({"engine":"stub","idle_seconds":2,"legs":[{"packets_in":1427,"bytes_in":84790,"packets_out":4,"bytes_out":336},{"packets_in":4,"bytes_in":336}]})";
};

struct Response {
  unsigned status = 0;
  std::string body;
  std::string content_type;

  boost::json::value json() const {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(body, ec);
    return ec ? boost::json::value() : parsed;
  }
};

struct CallsFixture : CoreFixture {
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::CallsAPI> calls;

  explicit CallsFixture(bool with_engine = true) {
    config->http_api_tokens = {Config::ApiToken{"admin-token", {"admin"}}, Config::ApiToken{"client-token", {"client"}}};

    if (with_engine) core->media_register(std::make_shared<StubEngine>());

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    router = std::make_shared<api::Router>(std::make_shared<api::BearerAuth>(config->http_api_tokens));
    calls = std::make_shared<api::CallsAPI>(logger, core, admin->executor());
    calls->register_routes(*router);

    admin->middlewares.push_back(router->middleware("/api/"));
    admin->middlewares.push_back(router->middleware("/metrics"));
    admin->start();
  }

  ~CallsFixture() { admin->stop(); }

  std::shared_ptr<Call> add_call(const std::string& id) {
    auto call = std::make_shared<Call>();
    call->id = id;
    call->state = Call::State::Connected;
    call->created_at = 1790000000;
    call->answered_at = 1790000005;

    auto& alice = call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
    alice.profile = media::Profile::WebRtc;
    call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));

    on_strand([&]() { core->call_register(call); });
    return call;
  }

  Response get(const std::string& target, const std::string& token = "admin-token") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + token);
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);
    socket.close(ec);

    return Response{response.result_int(), response.body(), std::string(response[http::field::content_type])};
  }
};

}  // namespace

TEST(CallsApiTest, ListsTheLiveCallsWithWhatTheEngineCarried) {
  CallsFixture f;
  f.add_call("call-one@example.com");

  const auto response = f.get("/api/v1/calls");
  ASSERT_EQ(response.status, 200u) << response.body;

  const auto calls = response.json().as_array();
  ASSERT_EQ(calls.size(), 1u);
  const auto& call = calls[0].as_object();

  EXPECT_EQ(call.at("id").as_string(), "call-one@example.com");
  EXPECT_EQ(call.at("state").as_string(), "Connected");
  EXPECT_EQ(call.at("created_at").as_string(), "2026-09-21T14:13:20Z");
  EXPECT_EQ(call.at("answered_at").as_string(), "2026-09-21T14:13:25Z");

  const auto& participants = call.at("participants").as_array();
  ASSERT_EQ(participants.size(), 2u);
  EXPECT_EQ(participants[0].as_object().at("identity").as_string(), "sip:alice@example.com");
  EXPECT_TRUE(participants[0].as_object().at("originator").as_bool());
  EXPECT_EQ(participants[0].as_object().at("profile").as_string(), "webrtc");
  EXPECT_TRUE(participants[1].as_object().at("profile").is_null()) << "a leg that has not described itself has no profile yet";

  const auto& media = call.at("media").as_object();
  EXPECT_EQ(media.at("engine").as_string(), "stub");
  EXPECT_EQ(media.at("idle_seconds").as_int64(), 2);

  const auto& legs = media.at("legs").as_array();
  ASSERT_EQ(legs.size(), 2u);
  EXPECT_EQ(legs[0].as_object().at("packets_in").as_int64(), 1427);
  EXPECT_EQ(legs[0].as_object().at("packets_out").as_int64(), 4);
  EXPECT_TRUE(legs[0].as_object().at("participant").is_null()) << "the engine cannot say which participant an end is";
  EXPECT_FALSE(legs[1].as_object().contains("packets_out")) << "what the engine did not report is not made up";
}

TEST(CallsApiTest, NoCallsIsAnEmptyList) {
  CallsFixture f;

  const auto response = f.get("/api/v1/calls");
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().as_array().empty());
}

// RFC 3261 25.1 lets a Call-ID hold a "/", so one arrives percent-encoded in the path and
// has to be matched as one segment and decoded only after.
TEST(CallsApiTest, OneCallByItsIdEvenWithASlashInIt) {
  CallsFixture f;
  f.add_call("a/b@example.com");

  const auto response = f.get("/api/v1/calls/a%2Fb%40example.com");
  ASSERT_EQ(response.status, 200u) << response.body;
  EXPECT_EQ(response.json().as_object().at("id").as_string(), "a/b@example.com");
}

TEST(CallsApiTest, ACallThatIsNotLiveIsNotFound) {
  CallsFixture f;

  EXPECT_EQ(f.get("/api/v1/calls/nobody%40example.com").status, 404u);
}

// A realm that does not anchor media has none for the engine to report on.
TEST(CallsApiTest, AnUnanchoredCallHasNoMedia) {
  CallsFixture f;
  auto call = f.add_call("plain@example.com");
  f.on_strand([&]() { call->media_policy.anchor = false; });

  const auto response = f.get("/api/v1/calls/plain%40example.com");
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().as_object().at("media").is_null());
}

TEST(CallsApiTest, WithNoEngineThereIsNoMedia) {
  CallsFixture f(/*with_engine=*/false);
  f.add_call("call-one@example.com");

  const auto response = f.get("/api/v1/calls");
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().as_array()[0].as_object().at("media").is_null());
}

// What is on a call is status, not provisioning: the same role as registrations, and
// nothing without a credential.
TEST(CallsApiTest, ReadingCallsTakesACredential) {
  CallsFixture f;

  EXPECT_EQ(f.get("/api/v1/calls", "").status, 401u);
  EXPECT_EQ(f.get("/api/v1/media", "").status, 401u);
}

TEST(CallsApiTest, TheMediaEngineDescribesItselfWithoutItsAddress) {
  CallsFixture f;

  const auto response = f.get("/api/v1/media");
  ASSERT_EQ(response.status, 200u) << response.body;

  const auto parsed = response.json();
  const auto& engine = parsed.as_object();
  EXPECT_EQ(engine.at("engine").as_string(), "stub");
  EXPECT_TRUE(engine.at("connected").as_bool());

  const auto& capabilities = engine.at("capabilities").as_array();
  ASSERT_EQ(capabilities.size(), 2u);
  EXPECT_EQ(capabilities[0].as_string(), "bridge");
  EXPECT_EQ(capabilities[1].as_string(), "record");
}

TEST(CallsApiTest, NoMediaEngineIsSaidPlainly) {
  CallsFixture f(/*with_engine=*/false);

  const auto response = f.get("/api/v1/media");
  ASSERT_EQ(response.status, 200u);
  EXPECT_TRUE(response.json().as_object().at("engine").is_null());
  EXPECT_FALSE(response.json().as_object().at("connected").as_bool());
}

// The Prometheus text exposition format, version 0.0.4.
TEST(CallsApiTest, MetricsAreInPrometheusTextFormat) {
  CallsFixture f;
  f.add_call("call-one@example.com");
  f.make_channel("192.0.2.10", nullptr, "udp");
  f.make_channel("192.0.2.11", nullptr, "tcp");
  f.settle();

  const auto response = f.get("/metrics");
  ASSERT_EQ(response.status, 200u) << response.body;
  EXPECT_EQ(response.content_type, "text/plain; version=0.0.4");

  const auto& body = response.body;
  EXPECT_NE(body.find("# TYPE athenasip_calls_active gauge\nathenasip_calls_active 1\n"), std::string::npos) << body;
  EXPECT_NE(body.find("athenasip_channels_open{transport=\"udp\"} 1\n"), std::string::npos) << body;
  EXPECT_NE(body.find("athenasip_channels_open{transport=\"tcp\"} 1\n"), std::string::npos) << body;
  EXPECT_NE(body.find("# TYPE athenasip_dialogs_active gauge\n"), std::string::npos) << body;
  EXPECT_NE(body.find("# TYPE athenasip_transactions_active gauge\n"), std::string::npos) << body;
  EXPECT_NE(body.find("# TYPE athenasip_media_packets_relayed_total counter\nathenasip_media_packets_relayed_total 123\n"), std::string::npos) << body;
}

// An engine that cannot count says nothing, rather than a zero that reads as "carried
// nothing".
TEST(CallsApiTest, AnEngineThatCannotCountIsLeftOutOfTheMetrics) {
  CallsFixture f(/*with_engine=*/false);

  const auto response = f.get("/metrics");
  ASSERT_EQ(response.status, 200u);
  EXPECT_EQ(response.body.find("athenasip_media_packets_relayed_total"), std::string::npos);
}

TEST(CallsApiTest, MetricsTakeACredential) {
  CallsFixture f;

  EXPECT_EQ(f.get("/metrics", "").status, 401u);
}
