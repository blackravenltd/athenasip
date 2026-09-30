//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "channel.h"
#include "core.h"
#include "datastores/memory_datastore.h"
#include "helpers/core_fixture_helper.h"
#include "helpers/recording_event_system_helper.h"
#include "helpers/sync_datastore_helper.h"
#include "mocks/connection_mock.h"
#include "timer_source.h"
#include "types/url.h"

using namespace athenasip;

namespace {

// A UDP flow is made by the first datagram from an address and has nothing to end it: no
// socket, no close, and no error on the read. Everything here is about what forgets one.
std::shared_ptr<Channel> flow(CoreFixture& f, const std::string& transport, const std::string& address, std::uint16_t port, bool reliable) {
  auto connection = std::make_shared<MockConnection>(transport, address, port);
  connection->reliable = reliable;
  connection->start();

  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);

  f.on_strand([&]() { f.core->channel_register(channel->flow_id(), channel); });
  f.settle();

  return channel;
}

}  // namespace

TEST(CoreFlowSweepTest, AUdpFlowThatHasGoneQuietIsForgotten) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  auto channel = flow(f, "udp", "198.51.100.7", 5060, /*reliable=*/false);
  const auto flow_id = channel->flow_id();

  ASSERT_NE(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);

  f.core->flow_sweep_start();

  // Past the timeout, on the injectable clock. Measuring this against the real one would
  // be five minutes of waiting.
  f.timers->advance(std::chrono::seconds(400));
  f.settle();

  EXPECT_EQ(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);
}

TEST(CoreFlowSweepTest, AUdpFlowStillInUseIsLeftAlone) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  auto channel = flow(f, "udp", "198.51.100.7", 5060, /*reliable=*/false);
  const auto flow_id = channel->flow_id();

  f.core->flow_sweep_start();

  // Quiet for a while, then used, then quiet again for less than the timeout. Neither
  // stretch on its own reaches it, and the flow is in use throughout.
  f.timers->advance(std::chrono::seconds(200));
  f.settle();
  channel->touch();

  f.timers->advance(std::chrono::seconds(200));
  f.settle();

  EXPECT_NE(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);
}

// A TCP, TLS or WebSocket flow ends when its socket does, and its silence means nothing: a
// phone that registered an hour ago and has said nothing since is still reachable down that
// socket, and closing it would take away the only path home a NATed client has.
TEST(CoreFlowSweepTest, AReliableFlowIsNeverSweptHoweverQuiet) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  auto channel = flow(f, "tcp", "198.51.100.8", 5060, /*reliable=*/true);
  const auto flow_id = channel->flow_id();

  f.core->flow_sweep_start();

  f.timers->advance(std::chrono::seconds(4000));
  f.settle();

  EXPECT_NE(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);
}

TEST(CoreFlowSweepTest, ATimeoutOfZeroForgetsNothing) {
  CoreFixture f;

  // What it did before this existed, for anyone who needs it back.
  f.config->sip_flow_idle_timeout = 0;

  auto channel = flow(f, "udp", "198.51.100.7", 5060, /*reliable=*/false);
  const auto flow_id = channel->flow_id();

  f.core->flow_sweep_start();

  f.timers->advance(std::chrono::seconds(100000));
  f.settle();

  EXPECT_NE(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);
}

// The point of the whole thing. The map is keyed by a remote address a datagram can claim
// to be from, so on a public listener an unbounded one is a way to grow a node's memory
// from off the network.
TEST(CoreFlowSweepTest, ManyOneShotFlowsDoNotAccumulate) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  for (std::uint16_t i = 0; i < 40; ++i) {
    flow(f, "udp", "198.51.100." + std::to_string(i + 10), 5060, /*reliable=*/false);
  }

  f.core->flow_sweep_start();
  f.timers->advance(std::chrono::seconds(400));
  f.settle();

  for (std::uint16_t i = 0; i < 40; ++i) {
    const auto address = "198.51.100." + std::to_string(i + 10);
    EXPECT_EQ(f.on_strand([&]() { return f.core->channel_find("udp", address, 5060); }), nullptr) << address;
  }
}

TEST(CoreFlowSweepTest, TheSweepKeepsRunning) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  f.core->flow_sweep_start();

  // A first pass that finds nothing must not be the last one: the flow below is made after
  // it, and something has to come back for it.
  f.timers->advance(std::chrono::seconds(400));
  f.settle();

  auto channel = flow(f, "udp", "198.51.100.7", 5060, /*reliable=*/false);
  const auto flow_id = channel->flow_id();

  f.timers->advance(std::chrono::seconds(400));
  f.settle();

  EXPECT_EQ(f.on_strand([&]() { return f.core->channel_find(flow_id); }), nullptr);
}

// docs/events.md promises a `closed` for a channel that goes, and T.O.M.S was told the
// channels topics carry registered and closed. Before this a UDP flow published
// "registered" and never anything else, so a consumer watching them saw only openings.
TEST(CoreFlowSweepTest, ForgettingAFlowSaysSoOnTheBus) {
  auto logger = std::make_shared<MockLogger>();
  auto config = std::make_shared<Config>(logger);
  config->sip_node_id = "test-node";
  config->sip_flow_idle_timeout = 300;

  auto datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
  SyncDatastore(datastore).connect();

  auto recorder = std::make_shared<RecordingEventSystem>();
  auto timers = std::make_shared<ManualTimerSource>();

  auto core = std::make_shared<Core>(logger, config, datastore, recorder);
  core->timer_source_set(timers);

  auto connection = std::make_shared<MockConnection>("udp", "198.51.100.7", 5060);
  connection->reliable = false;
  connection->start();

  auto channel = std::make_shared<Channel>(logger, core, connection);
  core->call_on_strand([&]() { core->channel_register(channel->flow_id(), channel); });

  core->flow_sweep_start();
  timers->advance(std::chrono::seconds(400));

  // The strand and the bus both settle on the global io_context.
  for (int i = 0; i < 32; ++i) core->call_on_strand([]() {});

  bool registered = false;
  bool closed = false;

  for (const auto& entry : recorder->published()) {
    if (entry.first.find("/channels/udp/198.51.100.7:5060") == std::string::npos) continue;

    if (entry.second.find("\"registered\"") != std::string::npos) registered = true;
    if (entry.second.find("\"closed\"") != std::string::npos) closed = true;
  }

  EXPECT_TRUE(registered);
  EXPECT_TRUE(closed) << "a UDP flow that is forgotten has to say so, or a consumer counts openings for ever";
}

// Closing a flow has to reach the connection, and for UDP that nearly did not happen.
// Channel::close() only tears a connection down if it says it is open, and UDPServer was
// the one server that never called start() on the connections it made - so an inbound UDP
// flow answered is_open() false for its whole life and was skipped. The flow left the
// channel registry and stayed in the server's map, and the next datagram from that address
// was handed to the closed connection instead of making a live one.
//
// Found by watching a live node rather than by reading: the sweep logged "Forgetting" and a
// second datagram from the same address produced no new endpoint.
TEST(CoreFlowSweepTest, ForgettingAFlowClosesItsConnection) {
  CoreFixture f;
  f.config->sip_flow_idle_timeout = 300;

  auto connection = std::make_shared<MockConnection>("udp", "198.51.100.7", 5060);
  connection->reliable = false;
  connection->start();

  ASSERT_TRUE(connection->is_open()) << "a live flow has to say it is open, or close() skips it";

  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&]() { f.core->channel_register(channel->flow_id(), channel); });
  f.settle();

  f.core->flow_sweep_start();
  f.timers->advance(std::chrono::seconds(400));
  f.settle();

  EXPECT_FALSE(connection->is_open()) << "the connection has to be closed, not just dropped from the registry";
}
