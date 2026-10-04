//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "core.h"

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <vector>

#include "channel.h"
#include "datastores/memory_datastore.h"
#include "events/local_event_system.h"
#include "headers/via_header.h"
#include "helpers/sync_datastore_helper.h"
#include "helpers/sync_event_system_helper.h"
#include "mocks/connection_mock.h"
#include "mocks/logger_mock.h"
#include "transactions/non_invite_server_transaction.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

namespace {

struct Fixture {
  std::shared_ptr<MockLogger> logger;
  std::shared_ptr<Config> config;
  std::shared_ptr<MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<events::LocalEventSystem> event_system;
  std::shared_ptr<SyncEventSystem> bus;
  std::shared_ptr<Core> core;
  std::shared_ptr<ManualTimerSource> timers = std::make_shared<ManualTimerSource>();

  Fixture() {
    logger = std::make_shared<MockLogger>();

    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();

    event_system = std::make_shared<events::LocalEventSystem>(logger);
    bus = std::make_shared<SyncEventSystem>(event_system);
    bus->connect();

    core = std::make_shared<Core>(logger, config, datastore, event_system);
    core->timer_source_set(timers);
  }

  // Drain the strand first: in-flight handlers hold the channel and connection being destroyed.
  ~Fixture() { settle(); }

  void settle(int rounds = 32) {
    for (int i = 0; i < rounds; ++i) core->call_on_strand([]() {});
  }

  // Starts an asynchronous Core call on the strand and blocks the test thread for its status.
  template <typename Start>
  plugins::Status await_on_strand(Start start) {
    std::promise<plugins::Status> promise;
    auto future = promise.get_future();

    core->post([&]() { start([&promise](plugins::Status status) { promise.set_value(std::move(status)); }); });

    return future.get();
  }

  bool register_binding(const std::shared_ptr<types::Subscriber>& subscriber, const std::shared_ptr<types::SIPUri>& contact,
                        const std::shared_ptr<Channel>& channel, std::uint32_t expires_seconds) {
    return await_on_strand(
               [&](plugins::StatusHandler handler) { core->subscriber_register(subscriber, contact, channel, expires_seconds, "", std::move(handler)); })
        .ok;
  }

  bool unregister_binding(const std::shared_ptr<types::Subscriber>& subscriber, const std::shared_ptr<types::SIPUri>& contact,
                          const std::shared_ptr<Channel>& channel) {
    return await_on_strand([&](plugins::StatusHandler handler) { core->subscriber_unregister(subscriber, contact, channel, std::move(handler)); }).ok;
  }

  // Core is strand-confined. call_on_strand runs inline when already on the strand, so nesting is safe.
  template <typename Fn>
  auto on_strand(Fn&& fn) {
    return core->call_on_strand(std::forward<Fn>(fn));
  }

  std::shared_ptr<Channel> make_channel(const std::string& remote_address, std::shared_ptr<MockConnection>* out = nullptr) {
    auto connection = std::make_shared<MockConnection>("tcp", remote_address);
    if (out) *out = connection;

    auto channel = std::make_shared<Channel>(logger, core, connection);
    on_strand([&channel]() { channel->start(); });
    return channel;
  }

  // A transaction wired as Core wires its own: terminating removes it from the registry.
  std::shared_ptr<transactions::TransactionBase> make_transaction(const std::string& key) {
    auto transaction = std::make_shared<transactions::NonInviteServerTransaction>(
        logger, key, true, transactions::Timers::from_config(*config), timers, [](std::shared_ptr<SIPMessage>) {}, [](std::shared_ptr<SIPMessage>) {});

    auto core_ptr = core;
    transaction->on_terminated([core_ptr](const std::string& id) { core_ptr->transaction_remove(id); });
    return transaction;
  }

  std::shared_ptr<types::Subscriber> seed_subscriber(uint64_t id, const std::string& uri) {
    auto realm = std::make_shared<types::Realm>("example.com");
    realm->id = 1;
    store->realm_create(realm);

    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = id;
    subscriber->identity = std::make_shared<types::SIPIdentity>(uri);
    subscriber->ha1 = "deadbeef";
    store->subscriber_create(subscriber);

    return subscriber;
  }
};

}  // namespace

// Terminating a transaction removes it from the table transaction_end_all is iterating.
TEST(CoreTest, TransactionEndAllClearsEveryTransaction) {
  Fixture f;

  std::vector<std::string> keys;
  for (int i = 0; i < 16; ++i) {
    const auto key = "z9hG4bK-" + std::to_string(i) + "|alice.example.com|OPTIONS";
    keys.push_back(key);

    auto transaction = f.make_transaction(key);
    f.on_strand([&]() { f.core->transaction_add(key, transaction); });
  }

  ASSERT_NE(f.on_strand([&]() { return f.core->transaction_get(keys.front()); }), nullptr);

  f.on_strand([&]() { f.core->transaction_end_all(); });

  for (const auto& key : keys) EXPECT_EQ(f.on_strand([&]() { return f.core->transaction_get(key); }), nullptr);
  EXPECT_EQ(f.on_strand([&]() { return f.core->transaction_count(); }), 0u);
}

TEST(CoreTest, TransactionEndAllOnAnEmptyRegistryIsFine) {
  Fixture f;
  EXPECT_NO_THROW(f.on_strand([&]() { f.core->transaction_end_all(); }));
}

TEST(CoreTest, TransactionAddAndRemoveRoundTrip) {
  Fixture f;

  const std::string key = "z9hG4bK-one|alice.example.com|OPTIONS";
  auto transaction = f.make_transaction(key);

  f.on_strand([&]() { f.core->transaction_add(key, transaction); });
  EXPECT_EQ(f.on_strand([&]() { return f.core->transaction_get(key); }), transaction);

  EXPECT_TRUE(f.on_strand([&]() { return f.core->transaction_remove(key); }));
  EXPECT_EQ(f.on_strand([&]() { return f.core->transaction_get(key); }), nullptr);

  // Removing an absent transaction reports failure.
  EXPECT_FALSE(f.on_strand([&]() { return f.core->transaction_remove(key); }));
}

// close() unregisters the channel, erasing from the map channel_close_all is iterating.
TEST(CoreTest, ChannelCloseAllClosesEveryChannel) {
  Fixture f;

  std::vector<std::shared_ptr<MockConnection>> connections;
  std::vector<std::shared_ptr<Channel>> channels;

  for (int i = 0; i < 16; ++i) {
    std::shared_ptr<MockConnection> connection;
    channels.push_back(f.make_channel("192.0.2." + std::to_string(i + 1), &connection));
    connections.push_back(connection);
  }

  f.on_strand([&]() { f.core->channel_close_all(); });

  for (const auto& connection : connections) EXPECT_EQ(connection->close_calls, 1);
  for (const auto& channel : channels) EXPECT_EQ(channel->state, Channel::State::Closed);
}

TEST(CoreTest, ChannelCloseAllOnAnEmptyRegistryIsFine) {
  Fixture f;
  EXPECT_NO_THROW(f.on_strand([&]() { f.core->channel_close_all(); }));
}

// A channel already closed is not closed again by channel_close_all.
TEST(CoreTest, ChannelCloseIsIdempotent) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.50", &connection);

  f.on_strand([&]() { channel->close(); });
  EXPECT_EQ(connection->close_calls, 1);

  f.on_strand([&]() { f.core->channel_close_all(); });
  EXPECT_EQ(connection->close_calls, 1);
}

// RFC 3261 10.3 step 7: a successful REGISTER stores the contact as a binding.
TEST(CoreTest, SubscriberRegisterStoresTheContact) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].contact->host, "192.0.2.10");
}

TEST(CoreTest, SubscriberRegisterIsRepeatable) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));
  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));

  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// RFC 5626: a binding records the flow it was learned over and the node holding that flow, the
// only route back to a client behind NAT.
TEST(CoreTest, SubscriberRegisterRecordsTheFlowItWasLearnedOver) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].flow_id, "tcp://192.0.2.10:5060");
  EXPECT_EQ(locations[0].node_id, "test-node");
}

// The recorded flow id is the channel registry's own key, so channel_find resolves it.
TEST(CoreTest, TheRecordedFlowIdIsWhatChannelFindAnswersTo) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].flow_id, channel->flow_id());
  EXPECT_EQ(locations[0].flow_id, Core::channel_key("TCP", "192.0.2.10", 5060));

  auto found = f.on_strand([&]() { return f.core->channel_find("tcp", "192.0.2.10", 5060); });
  EXPECT_EQ(found, channel);
}

// A binding learned over no channel (provisioned, or replayed by another node) records no flow.
TEST(CoreTest, SubscriberRegisterWithoutAChannelRecordsNoFlow) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");

  ASSERT_TRUE(f.register_binding(subscriber, contact, nullptr, 3600));

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_TRUE(locations[0].flow_id.empty());
  EXPECT_EQ(locations[0].node_id, "test-node");
}

// Unregistering removes the binding.
TEST(CoreTest, SubscriberUnregisterDropsTheBinding) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.register_binding(subscriber, contact, channel, 3600));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  ASSERT_TRUE(f.unregister_binding(subscriber, contact, channel));

  EXPECT_TRUE(f.store->location_list(7).empty());
}

// A socket belongs to its server's thread and is not safe for two threads at once, so the Core
// strand hands every read, write and close to the connection's executor.
TEST(CoreTest, ChannelStartsItsReadOnTheConnectionsExecutor) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("tcp", "192.0.2.10");
  connection->own_strand();

  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&channel]() { channel->start(); });
  f.settle();

  EXPECT_TRUE(connection->read_on_own_executor);
}

TEST(CoreTest, ChannelStartsItsWriteOnTheConnectionsExecutor) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("tcp", "192.0.2.10");
  connection->own_strand();

  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&channel]() { channel->start(); });

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");

  f.on_strand([&channel, &message]() { channel->send(message); });
  f.settle();

  ASSERT_EQ(connection->write_calls, 1);
  EXPECT_TRUE(connection->write_on_own_executor);
}

// Close runs on the connection's executor too: the server's thread may be inside the stream.
TEST(CoreTest, ChannelTearsTheConnectionDownOnItsOwnExecutor) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("tcp", "192.0.2.10");
  connection->own_strand();

  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&channel]() { channel->start(); });

  f.on_strand([&channel]() { channel->close(); });
  f.settle();

  ASSERT_EQ(connection->close_calls, 1);
  EXPECT_TRUE(connection->is_open_on_own_executor);
  EXPECT_TRUE(connection->shutdown_on_own_executor);
  EXPECT_TRUE(connection->close_on_own_executor);
}

// Writes on one connection are queued: two outstanding writes would interleave their bytes, and
// beast's WebSocket stream refuses the second.
TEST(CoreTest, ASecondMessageWaitsForTheFirstWriteToFinish) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.10", &connection);
  connection->defer_writes = true;

  f.on_strand([&channel]() {
    channel->write("FIRST\r\n");
    channel->write("SECOND\r\n");
  });
  f.settle();

  // Only the first is on the wire.
  EXPECT_EQ(connection->write_calls, 1);
  EXPECT_EQ(connection->written, "FIRST\r\n");

  // Completing it releases the second.
  f.on_strand([&connection]() { connection->complete_write(); });
  f.settle();

  EXPECT_EQ(connection->write_calls, 2);
  EXPECT_EQ(connection->written, "FIRST\r\nSECOND\r\n");
}

// async_write_some may take part of the buffer; a short write is resumed until the message is out.
TEST(CoreTest, AShortWriteIsResumedUntilTheMessageIsOut) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.10", &connection);
  connection->write_limit = 4;

  f.on_strand([&channel]() { channel->write("ABCDEFGHIJ"); });
  f.settle();

  EXPECT_EQ(connection->written, "ABCDEFGHIJ");
  EXPECT_EQ(connection->write_calls, 3);
}

// The proxy adds the Via (RFC 3261 16.6 step 8); the transport must not add one.
TEST(CoreTest, ChannelSendDoesNotAddAVia) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("udp", "192.0.2.10");
  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&]() { channel->start(); });

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");

  f.on_strand([&]() { channel->send(message); });

  EXPECT_FALSE(message->header->contains("Via"));
}

// RFC 3261 8.1.1.7: every Via carries a branch. The transaction layer sets it; the transport is the backstop.
TEST(CoreTest, ChannelSendFillsInAMissingBranch) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("udp", "192.0.2.10");
  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&]() { channel->start(); });

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");
  message->header->add("Via", std::make_shared<athenasip::headers::ViaHeader>("SIP/2.0/UDP 192.0.2.1:5060"));

  f.on_strand([&]() { channel->send(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_EQ(via->parameters["branch"].rfind("z9hG4bK", 0), 0u);
  EXPECT_EQ(message->branch, via->parameters["branch"]);
}

// An existing branch identifies the transaction and is left alone.
TEST(CoreTest, ChannelSendKeepsAnExistingBranch) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("udp", "192.0.2.10");
  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  f.on_strand([&]() { channel->start(); });

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");
  message->header->add("Via", std::make_shared<athenasip::headers::ViaHeader>("SIP/2.0/UDP 192.0.2.1:5060;branch=z9hG4bK-chosen-by-the-transaction"));

  f.on_strand([&]() { channel->send(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_EQ(via->parameters["branch"], "z9hG4bK-chosen-by-the-transaction");
}

// RFC 3261 18.2.1: a request whose Via sent-by differs from its source address gets a received parameter.
TEST(CoreTest, ChannelReceiveAddsReceivedWhenTheSourceDiffersFromTheVia) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.77", &connection);

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(
      "OPTIONS sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK-nat\r\n"
      "From: <sip:alice@example.com>;tag=alice\r\n"
      "To: <sip:bob@example.com>\r\n"
      "Call-ID: call-nat\r\n"
      "CSeq: 1 OPTIONS\r\n"
      "\r\n");

  f.on_strand([&]() { channel->receive(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_EQ(via->parameters["received"], "192.0.2.77");
}

// A Via whose sent-by matches the source gets no received parameter.
TEST(CoreTest, ChannelReceiveLeavesAMatchingViaAlone) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.78", &connection);

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(
      "OPTIONS sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP 192.0.2.78:5060;branch=z9hG4bK-direct\r\n"
      "From: <sip:alice@example.com>;tag=alice\r\n"
      "To: <sip:bob@example.com>\r\n"
      "Call-ID: call-direct\r\n"
      "CSeq: 1 OPTIONS\r\n"
      "\r\n");

  f.on_strand([&]() { channel->receive(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_FALSE(via->parameters.contains("received"));
}

// RFC 3581 section 4: an empty rport is filled with the source port; one with a value, or absent, is
// left alone.
TEST(CoreTest, ChannelReceiveFillsAnEmptyRport) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.79", &connection);

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(
      "OPTIONS sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP 10.0.0.5:5060;rport;branch=z9hG4bK-rport\r\n"
      "From: <sip:alice@example.com>;tag=alice\r\n"
      "To: <sip:bob@example.com>\r\n"
      "Call-ID: call-rport\r\n"
      "CSeq: 1 OPTIONS\r\n"
      "\r\n");

  f.on_strand([&]() { channel->receive(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_EQ(via->parameters["rport"], "5060");
}

TEST(CoreTest, ChannelReceiveDoesNotAddRportWhenItWasNotAskedFor) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.80", &connection);

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(
      "OPTIONS sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK-norport\r\n"
      "From: <sip:alice@example.com>;tag=alice\r\n"
      "To: <sip:bob@example.com>\r\n"
      "Call-ID: call-norport\r\n"
      "CSeq: 1 OPTIONS\r\n"
      "\r\n");

  f.on_strand([&]() { channel->receive(message); });

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_FALSE(via->parameters.contains("rport"));
}

// RFC 3261 10.3 step 7: removing a binding the registrar does not hold (expired, or from before a
// client restart) is not an error and is not logged as one.
TEST(CoreTest, RemovingABindingThatIsNotThereIsNotAnError) {
  Fixture f;
  auto subscriber = f.seed_subscriber(7, "sip:bob@example.com");
  auto channel = f.make_channel("192.0.2.30");

  f.unregister_binding(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.0.2.30:5060"), channel);

  for (const auto& line : f.logger->lines(loggers::LogLevel::ERROR)) ADD_FAILURE() << line;
}
