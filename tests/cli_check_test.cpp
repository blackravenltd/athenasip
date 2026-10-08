//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_check.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "cli.h"
#include "config.h"
#include "datastores/datastore_drivers.h"
#include "events/event_system_drivers.h"
#include "events/topics.h"
#include "global_io_context.h"
#include "media/media_engine_drivers.h"
#include "mocks/logger_mock.h"
#include "policy/policy_drivers.h"
#include "stun.h"

using namespace athenasip;

namespace {

// Runs a driver's connect to completion on a private io_context, as main does for --check.
plugins::Status wait_for(const std::function<void(plugins::Executor, plugins::StatusHandler)>& start) {
  boost::asio::io_context io;
  auto status = plugins::Status::failure("the driver never answered");

  start(io.get_executor(), [&status](plugins::Status answered) { status = std::move(answered); });
  io.run_for(std::chrono::seconds(5));

  return status;
}

struct CheckFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config = std::make_shared<Config>(logger);

  CheckFixture() {
    datastores::register_builtin_datastores(logger);
    events::register_builtin_event_systems(logger);
    media::register_builtin_media_engines(logger);
    policy::register_builtin_policies(logger);

    config->sip_node_id = "test-node";
    config->db_url = "memory://";
    config->events_url = "local://";
    config->media_url = "builtin://";
  }

  std::vector<cli::CheckLine> run() { return cli::check(logger, config, wait_for); }

  static const cli::CheckLine* find(const std::vector<cli::CheckLine>& lines, const std::string& what) {
    for (const auto& line : lines) {
      if (line.what == what) return &line;
    }
    return nullptr;
  }
};

}  // namespace

// The default drivers (memory, local, builtin) need nothing external and pass.
TEST(CliCheckTest, ANodeThatNeedsNothingExternalPasses) {
  CheckFixture f;

  const auto lines = f.run();

  ASSERT_NE(CheckFixture::find(lines, "datastore"), nullptr);
  ASSERT_NE(CheckFixture::find(lines, "events"), nullptr);
  ASSERT_NE(CheckFixture::find(lines, "media"), nullptr);
  EXPECT_TRUE(cli::passed(lines)) << cli::report(lines);
}

// An unknown driver scheme fails its own line; the remaining checks still run.
TEST(CliCheckTest, AnUnknownDriverFailsAndTheRestIsStillTried) {
  CheckFixture f;
  f.config->db_url = "carrier-pigeon://loft";

  const auto lines = f.run();

  const auto* datastore = CheckFixture::find(lines, "datastore");
  ASSERT_NE(datastore, nullptr);
  EXPECT_FALSE(datastore->ok);
  EXPECT_NE(datastore->detail.find("carrier-pigeon"), std::string::npos);

  const auto* media = CheckFixture::find(lines, "media");
  ASSERT_NE(media, nullptr);
  EXPECT_TRUE(media->ok);

  EXPECT_FALSE(cli::passed(lines));
}

// A certificate file a TLS listener needs and cannot read fails the check.
TEST(CliCheckTest, AMissingCertificateIsFound) {
  CheckFixture f;
  f.config->http_tls_enable = true;
  f.config->http_tls_cert_pem_filename = "/nowhere/admin.cer";
  f.config->http_tls_key_pem_filename = "/nowhere/admin.key";

  const auto lines = f.run();

  const auto* certificate = CheckFixture::find(lines, "http certificate");
  ASSERT_NE(certificate, nullptr);
  EXPECT_FALSE(certificate->ok);
  EXPECT_FALSE(cli::passed(lines));
}

// A cluster's first node has no peers, which is not a failure.
TEST(CliCheckTest, AClusterOfOneHasNoPeersAndStillPasses) {
  CheckFixture f;
  f.config->cluster_enable = true;

  const auto lines = f.run();

  const auto* peers = CheckFixture::find(lines, "peers");
  ASSERT_NE(peers, nullptr);
  EXPECT_TRUE(peers->ok);
}

// Passwords in driver URLs are redacted from the report.
TEST(CliCheckTest, APasswordInAUrlIsNotPrinted) {
  EXPECT_EQ(cli::redacted("memory://"), "memory://");

  const auto shown = cli::redacted("redis://:hunter2@127.0.0.1:6379/3");
  EXPECT_EQ(shown.find("hunter2"), std::string::npos) << shown;
  EXPECT_NE(shown.find("127.0.0.1"), std::string::npos) << shown;
}

TEST(CliCheckTest, TheFlagIsRead) {
  const char* argv[] = {"athenasip", "--check"};
  const auto options = cli::parse(2, const_cast<char**>(argv));

  EXPECT_TRUE(options.ok);
  EXPECT_TRUE(options.check);
}

namespace {

// A STUN server on loopback, on its own thread, that says whoever asks is at 203.0.113.7.
struct StunResponder {
  boost::asio::io_context io;
  boost::asio::ip::udp::socket socket{io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)};
  std::thread thread;

  StunResponder() {
    thread = std::thread([this]() {
      std::string request(2048, '\0');
      boost::asio::ip::udp::endpoint from;
      boost::system::error_code ec;
      socket.non_blocking(true);
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < until) {
        const auto size = socket.receive_from(boost::asio::buffer(request), from, 0, ec);
        if (!ec) {
          request.resize(size);
          if (auto response = stun::binding_response(request, boost::asio::ip::make_address("203.0.113.7"), 40000)) {
            socket.send_to(boost::asio::buffer(*response), from, 0, ec);
          }
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  ~StunResponder() { thread.join(); }

  std::string url() { return "stun:127.0.0.1:" + std::to_string(socket.local_endpoint().port()); }
};

}  // namespace

// The address a stun: server sees is reported, so an operator knows what sip.public_address should be.
TEST(CliCheckTest, ReportsTheAddressAStunServerSees) {
  CheckFixture f;
  StunResponder stun;
  f.config->ice_servers.push_back({stun.url()});

  const auto lines = f.run();

  const auto* address = CheckFixture::find(lines, "public address");
  ASSERT_NE(address, nullptr);
  EXPECT_TRUE(address->ok);
  EXPECT_NE(address->detail.find("203.0.113.7"), std::string::npos) << address->detail;
}

// A sip.public_address that is not what the world sees is the likely mistake, and fails the check.
TEST(CliCheckTest, APublicAddressTheStunServerDisagreesWithFails) {
  CheckFixture f;
  StunResponder stun;
  f.config->ice_servers.push_back({stun.url()});
  f.config->sip_public_address = "198.51.100.1";

  const auto lines = f.run();

  const auto* address = CheckFixture::find(lines, "public address");
  ASSERT_NE(address, nullptr);
  EXPECT_FALSE(address->ok) << address->detail;
}

// What peers found when they tried this node is reported: a listener none can reach fails, since peers will not
// forward to it. Runs against a real broker, which keeps the peer's status retained; skips without
// ATHENA_TEST_MQTT_URL.
TEST(CliCheckTest, SaysWhetherPeersReachThisNode) {
  const char* broker = std::getenv("ATHENA_TEST_MQTT_URL");
  if (broker == nullptr) GTEST_SKIP() << "ATHENA_TEST_MQTT_URL is not set";

  CheckFixture f;
  const auto url = std::string(broker) + "?prefix=check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "/";

  auto publisher = events::EventSystem::create_driver(f.logger, url);
  ASSERT_NE(publisher, nullptr);
  f.config->sip_node_id = "node-b-publisher";
  ASSERT_TRUE(publisher->configure(f.config->plugin_root("events", "mqtt"), *f.config));
  f.config->sip_node_id = "test-node";
  std::promise<plugins::Status> connecting;
  auto connected_future = connecting.get_future();
  publisher->connect(detail::get_global_io_context().get_executor(), [&connecting](plugins::Status status) { connecting.set_value(std::move(status)); });
  const auto connected = connected_future.get();
  ASSERT_TRUE(connected.ok) << connected.error;
  publisher->publish_state(events::topics::node_status("node-b"),
                           R"({"status":"ok","node":"node-b","version":"1.0.0","at":"2026-10-04T10:00:00Z","transports":[],)"
                           R"("cluster_probes":[{"node":"test-node","reached":false}]})");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  publisher->close();

  f.config->events_url = url;
  f.config->cluster_enable = true;

  // As main runs it: a driver that keeps working after connecting, as MQTT does, needs an executor that outlives
  // the connect.
  const auto lines = cli::check(f.logger, f.config, [](const std::function<void(plugins::Executor, plugins::StatusHandler)>& start) {
    std::promise<plugins::Status> promise;
    auto future = promise.get_future();
    start(detail::get_global_io_context().get_executor(), [&promise](plugins::Status status) { promise.set_value(std::move(status)); });
    return future.get();
  });

  const auto* reached = CheckFixture::find(lines, "reached by peers");
  ASSERT_NE(reached, nullptr) << cli::report(lines);
  EXPECT_FALSE(reached->ok);
  EXPECT_NE(reached->detail.find("node-b"), std::string::npos) << reached->detail;
}

// lua:// loads its scripts at --check, so one that does not compile is found there, with its file and line.
TEST(CliCheckTest, AScriptThatDoesNotCompileFailsTheCheckWithItsLine) {
  const auto root = std::filesystem::temp_directory_path() / ("athenasip-check-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root / "scripts");
  std::ofstream(root / "scripts" / "main.lua") << "-- routing\nfunction route(request\n";
  std::ofstream(root / "config.yaml") << "sip:\n  node_id: test-node\npolicy:\n  url: \"lua://\"\n  lua:\n    path: [\"" << (root / "scripts").string()
                                      << "\"]\n";

  CheckFixture f;
  ASSERT_TRUE(f.config->load_from_yaml((root / "config.yaml").string()));
  f.config->db_url = "memory://";
  f.config->events_url = "local://";
  f.config->media_url = "builtin://";

  const auto lines = f.run();
  std::filesystem::remove_all(root);

  const auto* policy = CheckFixture::find(lines, "policy");
  ASSERT_NE(policy, nullptr);
  EXPECT_FALSE(policy->ok);
  EXPECT_NE(policy->detail.find("main.lua:3"), std::string::npos) << policy->detail;
  EXPECT_FALSE(cli::passed(lines));
}

// With no main.lua of its own, lua:// runs the standard scripts, which load.
TEST(CliCheckTest, TheStandardScriptsPassTheCheck) {
  CheckFixture f;
  f.config->policy_url = "lua://";

  const auto lines = f.run();

  const auto* policy = CheckFixture::find(lines, "policy");
  ASSERT_NE(policy, nullptr);
  EXPECT_TRUE(policy->ok) << policy->detail;
}
