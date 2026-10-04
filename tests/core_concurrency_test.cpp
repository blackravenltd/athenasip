//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "call.h"
#include "channel.h"
#include "core.h"
#include "datastores/memory_datastore.h"
#include "events/local_event_system.h"
#include "helpers/sync_datastore_helper.h"
#include "helpers/sync_event_system_helper.h"
#include "mocks/connection_mock.h"
#include "mocks/logger_mock.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

// Each server runs its own io_context thread, so Core is reached from several threads and serialises
// them on a strand. These tests drive call_on_strand from many threads and check the registries.
namespace {

struct ConcurrentFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<Config> config;
  std::shared_ptr<MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<events::LocalEventSystem> event_system;
  std::shared_ptr<SyncEventSystem> bus;
  std::shared_ptr<Core> core;

  ConcurrentFixture() {
    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    event_system = std::make_shared<events::LocalEventSystem>(logger);
    bus = std::make_shared<SyncEventSystem>(event_system);
    bus->connect();

    core = std::make_shared<Core>(logger, config, datastore, event_system);
  }
};

constexpr int kThreads = 8;
constexpr int kIterations = 200;

}  // namespace

TEST(CoreConcurrencyTest, CallsSurviveConcurrentRegisterAndUnregister) {
  ConcurrentFixture f;
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&f, t]() {
      for (int i = 0; i < kIterations; ++i) {
        auto call = std::make_shared<Call>();
        call->id = "call-" + std::to_string(t) + "-" + std::to_string(i);

        f.core->call_on_strand([&]() { f.core->call_register(call); });
        f.core->call_on_strand([&]() { return f.core->call_get(call->id); });
        f.core->call_on_strand([&]() { f.core->call_unregister(call->id); });
      }
    });
  }

  for (auto& thread : threads) thread.join();

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kIterations; ++i) {
      EXPECT_EQ(f.core->call_on_strand([&]() { return f.core->call_get("call-" + std::to_string(t) + "-" + std::to_string(i)); }), nullptr);
    }
  }
}

TEST(CoreConcurrencyTest, CallGetRunsAlongsideRegistration) {
  ConcurrentFixture f;
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};

  std::thread reader([&]() {
    while (!stop.load()) {
      f.core->call_on_strand([&]() { return f.core->call_get("call-0"); });
      reads.fetch_add(1);
    }
  });

  for (int i = 0; i < kIterations; ++i) {
    auto call = std::make_shared<Call>();
    call->id = "call-" + std::to_string(i % 4);
    f.core->call_on_strand([&]() { f.core->call_register(call); });
    f.core->call_on_strand([&]() { f.core->call_unregister(call->id); });
  }

  stop.store(true);
  reader.join();

  EXPECT_GT(reads.load(), 0);
}

TEST(CoreConcurrencyTest, ChannelRegistrySurvivesConcurrentChannels) {
  ConcurrentFixture f;
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&f, t]() {
      for (int i = 0; i < 50; ++i) {
        auto connection = std::make_shared<MockConnection>("tcp", "192.0.2." + std::to_string(t + 1), static_cast<std::uint16_t>(5060 + i));
        auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
        f.core->call_on_strand([&]() { channel->start(); });
        f.core->call_on_strand([&]() { channel->close(); });
      }
    });
  }

  for (auto& thread : threads) thread.join();

  EXPECT_NO_THROW(f.core->call_on_strand([&]() { f.core->channel_close_all(); }));
}
