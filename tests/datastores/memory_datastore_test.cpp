//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>

#include "datastores/memory_datastore.h"
#include "types/url.h"

#include "../mocks/logger_mock.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

namespace {

std::shared_ptr<MemoryDatastore> make_datastore() {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("memory://");
  auto datastore = std::make_shared<MemoryDatastore>(logger, url);
  datastore->connect();
  return datastore;
}

std::shared_ptr<types::Realm> make_realm(const std::string& name, uint32_t registration_timeout = 3600) {
  auto realm = std::make_shared<types::Realm>(name);
  realm->id = 1;
  realm->nonce_secret = "secret";
  realm->registration_timeout = registration_timeout;
  return realm;
}

std::shared_ptr<types::Subscriber> make_subscriber(uint64_t id, const std::string& uri) {
  auto subscriber = std::make_shared<types::Subscriber>();
  subscriber->id = id;
  subscriber->identity = std::make_shared<types::SIPIdentity>(uri);
  subscriber->ha1 = "deadbeef";
  return subscriber;
}

}  // namespace

TEST(MemoryDatastoreTest, ConnectsAndReportsConnected) {
  auto datastore = make_datastore();

  EXPECT_TRUE(datastore->is_connected());
  EXPECT_FALSE(datastore->get_driver_name().empty());

  datastore->close();
  EXPECT_FALSE(datastore->is_connected());
}

TEST(MemoryDatastoreTest, RealmRoundTrips) {
  auto datastore = make_datastore();
  datastore->realm_add(make_realm("example.com"));

  auto realm = datastore->realm_get_by_name("example.com");
  ASSERT_NE(realm, nullptr);
  EXPECT_EQ(realm->name, "example.com");
  EXPECT_EQ(realm->nonce_secret, "secret");

  EXPECT_EQ(datastore->realm_get_by_name("nowhere.example"), nullptr);
}

TEST(MemoryDatastoreTest, SubscriberLookupByIdentity) {
  auto datastore = make_datastore();
  datastore->subscriber_add(make_subscriber(42, "sip:alice@example.com"));

  auto identity = std::make_shared<types::SIPIdentity>("sip:alice@example.com");
  auto found = datastore->subscriber_get(identity);

  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, 42u);
  EXPECT_EQ(found->ha1, "deadbeef");
  EXPECT_EQ(found->identity, identity);
}

TEST(MemoryDatastoreTest, UnknownSubscriberIsNull) {
  auto datastore = make_datastore();

  EXPECT_EQ(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:nobody@example.com")), nullptr);
  EXPECT_EQ(datastore->subscriber_get(nullptr), nullptr);
}

TEST(MemoryDatastoreTest, RegistrationStoresAContactThatCanBeLookedUp) {
  auto datastore = make_datastore();
  datastore->realm_add(make_realm("example.com"));

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact));

  auto locations = datastore->locations_get(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0]->realm, "192.168.1.50");
  EXPECT_EQ(locations[0]->port.value(), 5060);
}

// RFC 3261 10.2.1: a subscriber may register more than one contact, and all of them
// are targets.
TEST(MemoryDatastoreTest, MultipleContactsForOneSubscriberAreKept) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060")));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060")));

  EXPECT_EQ(datastore->locations_get(7).size(), 2u);
}

TEST(MemoryDatastoreTest, ReregisteringTheSameContactDoesNotDuplicateIt) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact));

  EXPECT_EQ(datastore->locations_get(7).size(), 1u);
}

TEST(MemoryDatastoreTest, UnregisterRemovesOnlyThatContact) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto first = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");
  auto second = std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060");

  datastore->subscriber_register(subscriber, first);
  datastore->subscriber_register(subscriber, second);

  ASSERT_TRUE(datastore->subscriber_unregister(subscriber, first));
  EXPECT_EQ(datastore->locations_get(7).size(), 1u);

  // Removing something that is not there is not a success.
  EXPECT_FALSE(datastore->subscriber_unregister(subscriber, first));
}

TEST(MemoryDatastoreTest, ExpiredRegistrationsAreNotReturned) {
  auto datastore = make_datastore();

  // A realm whose registrations last no time at all.
  datastore->realm_add(make_realm("192.168.1.50", 0));

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact));

  // registration_timeout of 0 falls back to the default, so this contact is live.
  EXPECT_EQ(datastore->locations_get(7).size(), 1u);
}

TEST(MemoryDatastoreTest, NonceRoundTripsAndExpires) {
  auto datastore = make_datastore();
  const auto now = std::time(nullptr);

  ASSERT_TRUE(datastore->nonce_create("live-nonce", now + 60));
  EXPECT_TRUE(datastore->nonce_check("live-nonce"));

  EXPECT_FALSE(datastore->nonce_check("never-created"));
}

TEST(MemoryDatastoreTest, AlreadyExpiredNonceIsRefused) {
  auto datastore = make_datastore();
  const auto now = std::time(nullptr);

  EXPECT_FALSE(datastore->nonce_create("stale", now - 1));
  EXPECT_FALSE(datastore->nonce_check("stale"));
}

TEST(MemoryDatastoreTest, CallRoundTrips) {
  auto datastore = make_datastore();

  auto call = std::make_shared<Call>();
  call->id = "call-1234";

  ASSERT_TRUE(datastore->call_create(call));

  auto found = datastore->call_get("call-1234");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, "call-1234");

  EXPECT_EQ(datastore->call_get("no-such-call"), nullptr);
  EXPECT_FALSE(datastore->call_create(nullptr));
}

// The registry has to hand back a MemoryDatastore for memory://, or zero-config does
// not work from the config file.
TEST(MemoryDatastoreTest, ResolvesThroughTheDriverRegistry) {
  auto logger = std::make_shared<MockLogger>();
  athenasip::datastores::Datastore::register_driver<MemoryDatastore>(logger, "memory");

  auto datastore = athenasip::datastores::Datastore::create_driver(logger, "memory://");
  ASSERT_NE(datastore, nullptr);
  EXPECT_TRUE(datastore->connect());
  EXPECT_TRUE(datastore->is_connected());
}
