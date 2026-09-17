//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "channel.h"
#include "core.h"
#include "headers/via_header.h"
#include "datastores/memory_datastore.h"
#include "events/local_event_system.h"

#include "mocks/connection_mock.h"
#include "mocks/logger_mock.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

namespace {

struct Fixture {
  std::shared_ptr<MockLogger> logger;
  std::shared_ptr<Config> config;
  std::shared_ptr<MemoryDatastore> datastore;
  std::shared_ptr<events::LocalEventSystem> event_system;
  std::shared_ptr<Core> core;

  Fixture() {
    logger = std::make_shared<MockLogger>();

    config = std::make_shared<Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    datastore->connect();

    event_system = std::make_shared<events::LocalEventSystem>(logger);
    event_system->connect();

    core = std::make_shared<Core>(logger, config, datastore, event_system);
  }

  std::shared_ptr<Channel> make_channel(const std::string& remote_address, std::shared_ptr<MockConnection>* out = nullptr) {
    auto connection = std::make_shared<MockConnection>("tcp", remote_address);
    if (out) *out = connection;

    auto channel = std::make_shared<Channel>(logger, core, connection);
    channel->start();
    return channel;
  }

  std::shared_ptr<types::Subscriber> seed_subscriber(uint64_t id, const std::string& uri) {
    auto realm = std::make_shared<types::Realm>("example.com");
    realm->id = 1;
    datastore->realm_add(realm);

    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->id = id;
    subscriber->identity = std::make_shared<types::SIPIdentity>(uri);
    subscriber->ha1 = "deadbeef";
    datastore->subscriber_add(subscriber);

    return subscriber;
  }
};

}  // namespace

// Regression: end() unregisters, which erases from the map being iterated.
TEST(CoreTest, TransactionEndAllClearsEveryTransaction) {
  Fixture f;

  std::vector<std::string> ids;
  for (int i = 0; i < 16; ++i) {
    const auto id = "transaction-" + std::to_string(i);
    ids.push_back(id);

    auto transaction = std::make_shared<Transaction>(f.logger, nullptr, f.core, Transaction::Direction::Incoming, id);
    ASSERT_TRUE(f.core->transaction_register(transaction));
  }

  ASSERT_NE(f.core->transaction_get(ids.front()), nullptr);

  f.core->transaction_end_all();

  for (const auto& id : ids) EXPECT_EQ(f.core->transaction_get(id), nullptr);
}

TEST(CoreTest, TransactionEndAllOnAnEmptyRegistryIsFine) {
  Fixture f;
  EXPECT_NO_THROW(f.core->transaction_end_all());
}

TEST(CoreTest, TransactionRegisterAndUnregisterRoundTrip) {
  Fixture f;

  auto transaction = std::make_shared<Transaction>(f.logger, nullptr, f.core, Transaction::Direction::Incoming, "t-1");
  ASSERT_TRUE(f.core->transaction_register(transaction));
  EXPECT_EQ(f.core->transaction_get("t-1"), transaction);

  EXPECT_TRUE(f.core->transaction_unregister("t-1"));
  EXPECT_EQ(f.core->transaction_get("t-1"), nullptr);

  // Unregistering something that is not there is not a success.
  EXPECT_FALSE(f.core->transaction_unregister("t-1"));
}

// Regression: close() unregisters, which erases from the map being iterated.
TEST(CoreTest, ChannelCloseAllClosesEveryChannel) {
  Fixture f;

  std::vector<std::shared_ptr<MockConnection>> connections;
  std::vector<std::shared_ptr<Channel>> channels;

  for (int i = 0; i < 16; ++i) {
    std::shared_ptr<MockConnection> connection;
    channels.push_back(f.make_channel("192.0.2." + std::to_string(i + 1), &connection));
    connections.push_back(connection);
  }

  f.core->channel_close_all();

  for (const auto& connection : connections) EXPECT_EQ(connection->close_calls, 1);
  for (const auto& channel : channels) EXPECT_EQ(channel->state, Channel::State::Closed);
}

TEST(CoreTest, ChannelCloseAllOnAnEmptyRegistryIsFine) {
  Fixture f;
  EXPECT_NO_THROW(f.core->channel_close_all());
}

// A channel closed on its own must not be closed a second time by the sweep.
TEST(CoreTest, ChannelCloseIsIdempotent) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.50", &connection);

  channel->close();
  EXPECT_EQ(connection->close_calls, 1);

  f.core->channel_close_all();
  EXPECT_EQ(connection->close_calls, 1);
}

// RFC 3261 10.3 step 7: a successful REGISTER stores the contact as a binding. Without
// it the registrar has nothing to route to.
TEST(CoreTest, SubscriberRegisterStoresTheContact) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.core->subscriber_register(subscriber, contact, channel));

  auto locations = f.datastore->locations_get(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0]->realm, "192.0.2.10");
}

TEST(CoreTest, SubscriberRegisterIsRepeatable) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.core->subscriber_register(subscriber, contact, channel));
  ASSERT_TRUE(f.core->subscriber_register(subscriber, contact, channel));

  EXPECT_EQ(f.datastore->locations_get(7).size(), 1u);
}

// The subscription is per-channel state. Once the subscriber is unregistered the node
// must stop delivering that subscriber's events down it.
TEST(CoreTest, SubscriberUnregisterDropsTheEventSubscription) {
  Fixture f;

  auto subscriber = f.seed_subscriber(7, "sip:alice@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  auto channel = f.make_channel("192.0.2.10");

  ASSERT_TRUE(f.core->subscriber_register(subscriber, contact, channel));
  ASSERT_NE(channel->_event_subscription, nullptr);

  ASSERT_TRUE(f.core->subscriber_unregister(subscriber, contact, channel));

  EXPECT_EQ(channel->_event_subscription, nullptr);
  EXPECT_TRUE(f.datastore->locations_get(7).empty());
}

// RFC 3261 18.1.1: the Via transport is the transport the request actually goes out
// on. This was hardcoded to TCP whatever the channel was.
TEST(CoreTest, ChannelSendSetsViaFromTheConnectionTransport) {
  Fixture f;

  for (const auto& transport : {"tcp", "udp", "tls", "ws", "wss"}) {
    auto connection = std::make_shared<MockConnection>(transport, "192.0.2.10");
    auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
    channel->start();

    auto message = std::make_shared<SIPMessage>();
    message->header = std::make_shared<SIPHeader>();
    message->header->type = SIPHeader::Type::Request;
    message->header->request_method = "OPTIONS";
    message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");

    channel->send(message);

    auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
    ASSERT_NE(via, nullptr);
    EXPECT_EQ(via->version, "SIP/2.0/" + Util::to_upper(transport)) << transport;
  }
}

// RFC 3261 8.1.1.7: the branch parameter must begin with the z9hG4bK magic cookie.
TEST(CoreTest, ChannelSendGeneratesAMagicCookieBranch) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("udp", "192.0.2.10");
  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  channel->start();

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");

  ASSERT_TRUE(message->branch.empty());
  channel->send(message);

  EXPECT_EQ(message->branch.rfind("z9hG4bK", 0), 0u);

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  ASSERT_NE(via, nullptr);
  EXPECT_EQ(via->parameters["branch"], message->branch);
}

// A branch the caller already chose is the transaction's, and must be left alone.
TEST(CoreTest, ChannelSendKeepsAnExistingBranch) {
  Fixture f;

  auto connection = std::make_shared<MockConnection>("udp", "192.0.2.10");
  auto channel = std::make_shared<Channel>(f.logger, f.core, connection);
  channel->start();

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>();
  message->header->type = SIPHeader::Type::Request;
  message->header->request_method = "OPTIONS";
  message->header->request_uri = std::make_shared<types::SIPUri>("sip:bob@example.com");
  message->branch = "z9hG4bK-chosen-by-the-transaction";

  channel->send(message);

  EXPECT_EQ(message->branch, "z9hG4bK-chosen-by-the-transaction");
}
