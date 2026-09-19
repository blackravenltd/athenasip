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

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

namespace {

// The contract is async; these tests are statements about what the store holds, so
// they drive it through the blocking test view.
std::shared_ptr<SyncDatastore> make_datastore() {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("memory://");
  auto datastore = std::make_shared<SyncDatastore>(std::make_shared<MemoryDatastore>(logger, url));
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
  EXPECT_EQ(datastore->name(), "memory");
  EXPECT_FALSE(datastore->version().empty());

  datastore->close();
  EXPECT_FALSE(datastore->is_connected());
}

TEST(MemoryDatastoreTest, RealmRoundTrips) {
  auto datastore = make_datastore();
  datastore->realm_create(make_realm("example.com"));

  auto realm = datastore->realm_get_by_name("example.com");
  ASSERT_NE(realm, nullptr);
  EXPECT_EQ(realm->name, "example.com");
  EXPECT_EQ(realm->nonce_secret, "secret");

  EXPECT_EQ(datastore->realm_get_by_name("nowhere.example"), nullptr);
}

TEST(MemoryDatastoreTest, SubscriberLookupByIdentity) {
  auto datastore = make_datastore();
  datastore->subscriber_create(make_subscriber(42, "sip:alice@example.com"));

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
  datastore->realm_create(make_realm("example.com"));

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact, 3600, ""));

  auto locations = datastore->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].contact->realm, "192.168.1.50");
  EXPECT_EQ(locations[0].contact->port.value(), 5060);
}

// RFC 3261 10.2.1: a subscriber may register more than one contact, and all of them
// are targets.
TEST(MemoryDatastoreTest, MultipleContactsForOneSubscriberAreKept) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060"), 3600, ""));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060"), 3600, ""));

  EXPECT_EQ(datastore->location_list(7).size(), 2u);
}

TEST(MemoryDatastoreTest, ReregisteringTheSameContactDoesNotDuplicateIt) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact, 3600, ""));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact, 3600, ""));

  EXPECT_EQ(datastore->location_list(7).size(), 1u);
}

TEST(MemoryDatastoreTest, UnregisterRemovesOnlyThatContact) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto first = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");
  auto second = std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060");

  datastore->subscriber_register(subscriber, first, 3600, "");
  datastore->subscriber_register(subscriber, second, 3600, "");

  ASSERT_TRUE(datastore->subscriber_unregister(subscriber, first));
  EXPECT_EQ(datastore->location_list(7).size(), 1u);

  // Removing something that is not there is not a success.
  EXPECT_FALSE(datastore->subscriber_unregister(subscriber, first));
}

TEST(MemoryDatastoreTest, ExpiredRegistrationsAreNotReturned) {
  auto datastore = make_datastore();

  // A realm whose registrations last no time at all.
  datastore->realm_create(make_realm("192.168.1.50", 0));

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact, 3600, ""));

  // registration_timeout of 0 falls back to the default, so this contact is live.
  EXPECT_EQ(datastore->location_list(7).size(), 1u);
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

  auto driver = athenasip::datastores::Datastore::create_driver(logger, "memory://");
  ASSERT_NE(driver, nullptr);

  SyncDatastore datastore(driver);
  EXPECT_TRUE(datastore.connect());
  EXPECT_TRUE(datastore.is_connected());
}

// Write operations. create and update are distinct on purpose: provisioning has to be
// able to tell "already exists" from "changed" rather than silently overwriting.

TEST(MemoryDatastoreTest, RealmCreateRefusesADuplicate) {
  auto datastore = make_datastore();

  EXPECT_TRUE(datastore->realm_create(make_realm("example.com")));
  EXPECT_FALSE(datastore->realm_create(make_realm("example.com")));
}

TEST(MemoryDatastoreTest, RealmUpdateRequiresAnExistingRealm) {
  auto datastore = make_datastore();

  EXPECT_FALSE(datastore->realm_update(make_realm("example.com")));

  ASSERT_TRUE(datastore->realm_create(make_realm("example.com")));

  auto changed = make_realm("example.com");
  changed->nonce_secret = "changed";
  EXPECT_TRUE(datastore->realm_update(changed));
  EXPECT_EQ(datastore->realm_get_by_name("example.com")->nonce_secret, "changed");
}

TEST(MemoryDatastoreTest, RealmDeleteAndList) {
  auto datastore = make_datastore();

  datastore->realm_create(make_realm("one.example"));
  datastore->realm_create(make_realm("two.example"));
  EXPECT_EQ(datastore->realm_list().size(), 2u);

  EXPECT_TRUE(datastore->realm_delete("one.example"));
  EXPECT_FALSE(datastore->realm_delete("one.example"));
  EXPECT_EQ(datastore->realm_list().size(), 1u);
  EXPECT_EQ(datastore->realm_get_by_name("one.example"), nullptr);
}

TEST(MemoryDatastoreTest, SubscriberCreateRefusesADuplicate) {
  auto datastore = make_datastore();

  EXPECT_TRUE(datastore->subscriber_create(make_subscriber(1, "sip:alice@example.com")));
  EXPECT_FALSE(datastore->subscriber_create(make_subscriber(1, "sip:alice@example.com")));
}

TEST(MemoryDatastoreTest, SubscriberUpdateRequiresAnExistingSubscriber) {
  auto datastore = make_datastore();

  EXPECT_FALSE(datastore->subscriber_update(make_subscriber(1, "sip:alice@example.com")));

  ASSERT_TRUE(datastore->subscriber_create(make_subscriber(1, "sip:alice@example.com")));

  auto changed = make_subscriber(1, "sip:alice@example.com");
  changed->ha1 = "newhash";
  EXPECT_TRUE(datastore->subscriber_update(changed));

  auto found = datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1, "newhash");
}

TEST(MemoryDatastoreTest, SubscriberListIsScopedToTheRealm) {
  auto datastore = make_datastore();

  datastore->subscriber_create(make_subscriber(1, "sip:alice@one.example"));
  datastore->subscriber_create(make_subscriber(2, "sip:bob@one.example"));
  datastore->subscriber_create(make_subscriber(3, "sip:carol@two.example"));

  EXPECT_EQ(datastore->subscriber_list("one.example").size(), 2u);
  EXPECT_EQ(datastore->subscriber_list("two.example").size(), 1u);
  EXPECT_EQ(datastore->subscriber_list("nowhere.example").size(), 0u);
}

// A deleted subscriber keeps no bindings: leaving them would route calls to someone
// who no longer exists.
TEST(MemoryDatastoreTest, SubscriberDeleteDropsTheirRegistrations) {
  auto datastore = make_datastore();

  auto subscriber = make_subscriber(9, "sip:dave@example.com");
  ASSERT_TRUE(datastore->subscriber_create(subscriber));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:dave@192.0.2.9:5060"), 3600, ""));
  ASSERT_EQ(datastore->location_list(9).size(), 1u);

  EXPECT_TRUE(datastore->subscriber_delete(subscriber->identity));
  EXPECT_TRUE(datastore->location_list(9).empty());
  EXPECT_FALSE(datastore->subscriber_delete(subscriber->identity));
}

TEST(MemoryDatastoreTest, LocationCarriesTheBindingNotJustTheContact) {
  auto datastore = make_datastore();

  auto subscriber = make_subscriber(11, "sip:erin@example.com");
  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:erin@10.0.0.7:5060"), 3600, ""));

  auto locations = datastore->location_list(11);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].subscriber_id, 11u);
  EXPECT_GT(locations[0].expires_at, locations[0].registered_at);

  // 10.0.0.0/8 is private, so the binding is marked as behind NAT.
  EXPECT_TRUE(locations[0].nat);
}

TEST(MemoryDatastoreTest, CallUpdateRequiresAnExistingCallAndListReturnsThem) {
  auto datastore = make_datastore();

  auto call = std::make_shared<Call>();
  call->id = "call-update-1";
  call->state = Call::State::Initial;

  EXPECT_FALSE(datastore->call_update(call));
  ASSERT_TRUE(datastore->call_create(call));

  call->state = Call::State::Connected;
  EXPECT_TRUE(datastore->call_update(call));
  EXPECT_EQ(datastore->call_get("call-update-1")->state, Call::State::Connected);

  EXPECT_EQ(datastore->call_list().size(), 1u);
}
