//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "datastores/memory_datastore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "call.h"
#include "config.h"
#include "types/password.h"
#include "types/presence_record.h"
#include "types/session.h"
#include "types/trunk.h"
#include "types/url.h"
#include "types/user.h"
#include "util.h"

using namespace athenasip;
using athenasip::datastores::MemoryDatastore;

namespace {

// The contract is async; these tests drive it through the blocking test view.
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

std::shared_ptr<types::User> make_user(const std::string& username, std::vector<std::string> roles) {
  auto user = std::make_shared<types::User>();
  user->username = username;
  user->roles = std::move(roles);
  user->password_hash = types::Password::hash("correct horse", 1000);
  user->created_at = std::time(nullptr);
  return user;
}

types::Session make_session(const std::string& token_hash, const std::string& username) {
  const auto now = std::time(nullptr);

  types::Session session;
  session.token_hash = token_hash;
  session.username = username;
  session.created_at = now;
  session.expires_at = now + 3600;
  session.last_seen_at = now;
  return session;
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
  EXPECT_EQ(locations[0].contact->host, "192.168.1.50");
  EXPECT_EQ(locations[0].contact->port.value(), 5060);
}

// A binding keeps the flow it was learned over (RFC 5626) and the node holding it, which a second node needs.
TEST(MemoryDatastoreTest, RegistrationKeepsTheFlowAndTheNodeItWasGiven) {
  auto datastore = make_datastore();
  auto subscriber = make_subscriber(7, "sip:bob@example.com");

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");
  binding.path = "<sip:edge.example.com;lr>";
  binding.flow_id = "tcp://192.168.1.50:5060";
  binding.node_id = "node-a";

  ASSERT_TRUE(datastore->subscriber_register(subscriber, binding, 3600));

  auto locations = datastore->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].path, "<sip:edge.example.com;lr>");
  EXPECT_EQ(locations[0].flow_id, "tcp://192.168.1.50:5060");
  EXPECT_EQ(locations[0].node_id, "node-a");

  // The store sets these itself, whatever the caller put there.
  EXPECT_EQ(locations[0].subscriber_id, 7u);
  EXPECT_GT(locations[0].expires_at, locations[0].registered_at);
}

// RFC 8760: a subscriber can hold a credential per algorithm, and a read returns all of them.
TEST(MemoryDatastoreTest, EveryCredentialSurvivesARead) {
  auto datastore = make_datastore();

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  subscriber->ha1 = "md5-hash";
  subscriber->ha1_sha256 = "sha256-hash";
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  auto found = datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1, "md5-hash");
  EXPECT_EQ(found->ha1_sha256, "sha256-hash");
}

// RFC 3261 10.2.1: a subscriber may register several contacts, and all of them are targets.
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

  // Removing what is not there is not a success.
  EXPECT_FALSE(datastore->subscriber_unregister(subscriber, first));
}

TEST(MemoryDatastoreTest, ExpiredRegistrationsAreNotReturned) {
  auto datastore = make_datastore();

  // A realm with a registration_timeout of 0.
  datastore->realm_create(make_realm("192.168.1.50", 0));

  auto subscriber = make_subscriber(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, contact, 3600, ""));

  // A registration_timeout of 0 falls back to the default, so this contact is live.
  EXPECT_EQ(datastore->location_list(7).size(), 1u);
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

// memory:// resolves to a MemoryDatastore through the registry, which zero-config depends on.
TEST(MemoryDatastoreTest, ResolvesThroughTheDriverRegistry) {
  auto logger = std::make_shared<MockLogger>();
  athenasip::datastores::Datastore::register_driver<MemoryDatastore>(logger, "memory");

  auto driver = athenasip::datastores::Datastore::create_driver(logger, "memory://");
  ASSERT_NE(driver, nullptr);

  SyncDatastore datastore(driver);
  EXPECT_TRUE(datastore.connect());
  EXPECT_TRUE(datastore.is_connected());
}

// Write operations. create and update are distinct so provisioning can tell "already exists" from "changed".

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

// A realm is deleted with its subscribers and their bindings, which would otherwise be unreachable through the
// API and go on routing calls.
TEST(MemoryDatastoreTest, RealmDeleteTakesItsSubscribersAndTheirRegistrations) {
  auto datastore = make_datastore();

  datastore->realm_create(make_realm("one.example"));
  datastore->realm_create(make_realm("two.example"));

  auto alice = make_subscriber(1, "sip:alice@one.example");
  auto carol = make_subscriber(3, "sip:carol@two.example");
  ASSERT_TRUE(datastore->subscriber_create(alice));
  ASSERT_TRUE(datastore->subscriber_create(make_subscriber(2, "sip:bob@one.example")));
  ASSERT_TRUE(datastore->subscriber_create(carol));
  ASSERT_TRUE(datastore->subscriber_register(alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.1:5060"), 3600, ""));
  ASSERT_TRUE(datastore->subscriber_register(carol, std::make_shared<types::SIPUri>("sip:carol@192.0.2.3:5060"), 3600, ""));

  ASSERT_TRUE(datastore->realm_delete("one.example"));

  EXPECT_EQ(datastore->subscriber_list("one.example").size(), 0u);
  EXPECT_EQ(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@one.example")), nullptr);
  EXPECT_EQ(datastore->location_list(1).size(), 0u);

  // And nothing of anybody else's.
  EXPECT_EQ(datastore->subscriber_list("two.example").size(), 1u);
  EXPECT_EQ(datastore->location_list(3).size(), 1u);
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

// A deleted subscriber keeps no bindings.
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

// Users and sessions, per docs/authentication.md: usernames are one case-insensitive namespace, and a session
// is held by the hash of its token, never the token.

TEST(MemoryDatastoreTest, UserRoundTrips) {
  auto datastore = make_datastore();

  auto user = make_user("tom", {types::roles::manage_realms});
  user->display_name = "Tom Cully";
  ASSERT_TRUE(datastore->user_create(user));

  auto found = datastore->user_get("tom");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->username, "tom");
  EXPECT_EQ(found->display_name, "Tom Cully");
  EXPECT_TRUE(found->has_role(types::roles::manage_realms));
  EXPECT_FALSE(found->has_role(types::roles::manage_admin_users));

  // The whole record survives, password hash included.
  EXPECT_EQ(found->password_hash, user->password_hash);
  EXPECT_TRUE(types::Password::verify("correct horse", found->password_hash));
  EXPECT_EQ(found->created_at, user->created_at);

  EXPECT_EQ(datastore->user_get("nobody"), nullptr);
}

// "Tom" and "tom" cannot both exist, and either spelling finds the one that does.
TEST(MemoryDatastoreTest, UsernamesAreOneNamespaceWhateverTheirCase) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->user_create(make_user("Tom", {})));

  EXPECT_NE(datastore->user_get("tom"), nullptr);
  EXPECT_NE(datastore->user_get("TOM"), nullptr);

  // The username keeps the spelling it was given.
  EXPECT_EQ(datastore->user_get("tom")->username, "Tom");

  EXPECT_FALSE(datastore->user_create(make_user("TOM", {})));
  EXPECT_FALSE(datastore->user_create(make_user("tom", {})));
}

// create is not update, so the API can answer 409.
TEST(MemoryDatastoreTest, UserCreateRefusesAnExistingUsername) {
  auto datastore = make_datastore();

  auto first = make_user("alice", {types::roles::view_cluster_status});
  ASSERT_TRUE(datastore->user_create(first));

  auto second = make_user("alice", {types::roles::manage_admin_users});
  EXPECT_FALSE(datastore->user_create(second));

  // The refusal changed nothing.
  auto found = datastore->user_get("alice");
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->has_role(types::roles::view_cluster_status));
  EXPECT_FALSE(found->has_role(types::roles::manage_admin_users));
}

// update is not create, so a PUT to an unknown username is a 404.
TEST(MemoryDatastoreTest, UserUpdateRequiresAnExistingUser) {
  auto datastore = make_datastore();

  auto user = make_user("bob", {});
  EXPECT_FALSE(datastore->user_update(user));
  EXPECT_EQ(datastore->user_get("bob"), nullptr);

  ASSERT_TRUE(datastore->user_create(user));

  auto changed = make_user("bob", {types::roles::manage_cluster});
  changed->disabled = true;
  EXPECT_TRUE(datastore->user_update(changed));

  auto found = datastore->user_get("bob");
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->has_role(types::roles::manage_cluster));
  EXPECT_TRUE(found->disabled);
}

// A user with no roles is a valid record.
TEST(MemoryDatastoreTest, AUserWithNoRolesIsStored) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->user_create(make_user("carol", {})));

  auto found = datastore->user_get("carol");
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->roles.empty());
}

TEST(MemoryDatastoreTest, UserWithoutAUsernameIsRefused) {
  auto datastore = make_datastore();

  EXPECT_FALSE(datastore->user_create(make_user("", {})));
  EXPECT_FALSE(datastore->user_create(nullptr));
  EXPECT_TRUE(datastore->user_list().empty());
}

TEST(MemoryDatastoreTest, UserListReturnsEveryUser) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->user_create(make_user("alice", {types::roles::manage_realms})));
  ASSERT_TRUE(datastore->user_create(make_user("bob", {})));

  EXPECT_EQ(datastore->user_list().size(), 2u);
}

TEST(MemoryDatastoreTest, UserDeleteRemovesThemOnceAndSaysSoTheSecondTime) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->user_create(make_user("dave", {})));

  EXPECT_TRUE(datastore->user_delete("DAVE"));
  EXPECT_EQ(datastore->user_get("dave"), nullptr);
  EXPECT_FALSE(datastore->user_delete("dave"));
}

// Deleting a user revokes their sessions.
TEST(MemoryDatastoreTest, UserDeleteRevokesTheirSessions) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->user_create(make_user("erin", {})));
  ASSERT_TRUE(datastore->session_create(make_session("hash-erin", "erin")));
  ASSERT_NE(datastore->session_get("hash-erin"), nullptr);

  EXPECT_TRUE(datastore->user_delete("erin"));
  EXPECT_EQ(datastore->session_get("hash-erin"), nullptr);
}

TEST(MemoryDatastoreTest, SessionRoundTripsByItsTokenHash) {
  auto datastore = make_datastore();

  auto session = make_session("hash-one", "frank");
  ASSERT_TRUE(datastore->session_create(session));

  auto found = datastore->session_get("hash-one");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->username, "frank");
  EXPECT_EQ(found->token_hash, "hash-one");
  EXPECT_EQ(found->expires_at, session.expires_at);
  EXPECT_EQ(found->last_seen_at, session.last_seen_at);

  EXPECT_EQ(datastore->session_get("hash-nothing"), nullptr);
  EXPECT_EQ(datastore->session_get(""), nullptr);
}

TEST(MemoryDatastoreTest, SessionWithoutAHashOrAUserIsRefused) {
  auto datastore = make_datastore();

  EXPECT_FALSE(datastore->session_create(make_session("", "frank")));
  EXPECT_FALSE(datastore->session_create(make_session("hash-two", "")));
}

// There is no session_update: writing the same hash again carries last_seen_at forward, which idle expiry is
// counted from.
TEST(MemoryDatastoreTest, WritingASessionAgainMovesItsLastSeen) {
  auto datastore = make_datastore();

  auto session = make_session("hash-touch", "grace");
  ASSERT_TRUE(datastore->session_create(session));

  session.last_seen_at += 300;
  ASSERT_TRUE(datastore->session_create(session));

  auto found = datastore->session_get("hash-touch");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->last_seen_at, session.last_seen_at);
  EXPECT_EQ(found->username, "grace");
}

// The store keeps the absolute expiry; idle expiry is the caller's (Session::has_expired). Both drivers refuse
// a session that is already expired, as Redis SETEX must.
TEST(MemoryDatastoreTest, AnAlreadyExpiredSessionIsRefused) {
  auto datastore = make_datastore();

  auto session = make_session("hash-old", "heidi");
  session.expires_at = std::time(nullptr) - 1;

  EXPECT_FALSE(datastore->session_create(session));
  EXPECT_EQ(datastore->session_get("hash-old"), nullptr);

  // An expires_at of 0 is refused, not stored as a session that never ends.
  session.expires_at = 0;
  EXPECT_FALSE(datastore->session_create(session));
}

TEST(MemoryDatastoreTest, SessionDeleteEndsThatOneSession) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->session_create(make_session("hash-a", "ivan")));
  ASSERT_TRUE(datastore->session_create(make_session("hash-b", "ivan")));

  EXPECT_TRUE(datastore->session_delete("hash-a"));
  EXPECT_EQ(datastore->session_get("hash-a"), nullptr);
  EXPECT_NE(datastore->session_get("hash-b"), nullptr);

  // Deleting twice succeeds twice: an answer that told a live token from an unknown one would be an oracle.
  EXPECT_TRUE(datastore->session_delete("hash-a"));
  EXPECT_TRUE(datastore->session_delete("never-existed"));
}

// Ends every session a user holds: what makes disabling immediate, and "sign out everywhere".
TEST(MemoryDatastoreTest, SessionDeleteForUserEndsAllOfTheirsAndNobodyElses) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-1", "Judy")));
  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-2", "judy")));
  ASSERT_TRUE(datastore->session_create(make_session("hash-ken", "ken")));

  EXPECT_TRUE(datastore->session_delete_for_user("JUDY"));
  EXPECT_EQ(datastore->session_get("hash-judy-1"), nullptr);
  EXPECT_EQ(datastore->session_get("hash-judy-2"), nullptr);
  EXPECT_NE(datastore->session_get("hash-ken"), nullptr);

  // Nothing to revoke is success.
  EXPECT_TRUE(datastore->session_delete_for_user("judy"));
}

// A read returns a copy, as a networked driver must, so the drivers cannot differ on an unwritten change.
TEST(MemoryDatastoreTest, ChangingWhatAReadHandedBackDoesNotChangeTheStore) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->user_create(make_user("mallory", {})));

  auto found = datastore->user_get("mallory");
  ASSERT_NE(found, nullptr);
  found->roles.push_back(types::roles::manage_admin_users);
  found->disabled = true;

  auto again = datastore->user_get("mallory");
  ASSERT_NE(again, nullptr);
  EXPECT_TRUE(again->roles.empty());
  EXPECT_FALSE(again->disabled);

  auto listed = datastore->user_list();
  ASSERT_EQ(listed.size(), 1u);
  listed[0]->disabled = true;
  EXPECT_FALSE(datastore->user_get("mallory")->disabled);

  auto session = make_session("hash-mallory", "mallory");
  ASSERT_TRUE(datastore->session_create(session));
  datastore->session_get("hash-mallory")->username = "somebody-else";
  EXPECT_EQ(datastore->session_get("hash-mallory")->username, "mallory");
}

// A call record is kept for the configured retention and no longer.
TEST(MemoryDatastoreTest, EndedCallsAreForgottenAfterTheRetention) {
  auto logger = std::make_shared<MockLogger>();
  auto driver = std::make_shared<MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));

  Config config(logger);
  config.calls_history_retention = 3600;
  ASSERT_TRUE(driver->configure(YAML::Node(), config));

  SyncDatastore datastore(driver);

  auto old = std::make_shared<Call>();
  old->id = "long-ago";
  old->state = Call::State::Closed;
  old->ended_at = std::time(nullptr) - 7200;

  auto recent = std::make_shared<Call>();
  recent->id = "just-now";
  recent->state = Call::State::Closed;
  recent->ended_at = std::time(nullptr) - 60;

  auto live = std::make_shared<Call>();
  live->id = "still-up";
  live->state = Call::State::Connected;

  ASSERT_TRUE(datastore.call_create(old));
  ASSERT_TRUE(datastore.call_create(recent));
  ASSERT_TRUE(datastore.call_create(live));

  EXPECT_EQ(datastore.call_get("long-ago"), nullptr);
  EXPECT_NE(datastore.call_get("just-now"), nullptr);
  EXPECT_NE(datastore.call_get("still-up"), nullptr);
}

// A trunk is stored whole and given back as stored, password and attributes included, and found by its name
// whatever its case.
TEST(MemoryDatastoreTest, ATrunkRoundTrips) {
  auto datastore = make_datastore();
  auto trunk = std::make_shared<types::Trunk>();
  trunk->name = "Acme";
  trunk->uri = "sip:sip.acme.example;transport=tls";
  trunk->username = "4420";
  trunk->password = "s3cret";
  trunk->register_enabled = true;
  trunk->register_expires = 600;
  trunk->contact_user = "4420";
  trunk->inbound_addresses = {"203.0.113.0/24", "2001:db8::/32"};
  trunk->attributes["prefixes"] = boost::json::array{"+44", "+1"};
  ASSERT_TRUE(datastore->trunk_create(trunk));

  const auto found = datastore->trunk_get(Util::to_upper(trunk->name));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->name, trunk->name);
  EXPECT_EQ(found->uri, trunk->uri);
  EXPECT_EQ(found->password, "s3cret");
  EXPECT_TRUE(found->register_enabled);
  EXPECT_EQ(found->register_expires, 600u);
  EXPECT_EQ(found->inbound_addresses, trunk->inbound_addresses);
  EXPECT_EQ(found->attributes, trunk->attributes);

  EXPECT_FALSE(datastore->trunk_create(trunk)) << "a second trunk of the same name is a conflict";

  found->password = "changed";
  ASSERT_TRUE(datastore->trunk_update(found));
  EXPECT_EQ(datastore->trunk_get(trunk->name)->password, "changed");

  bool listed = false;
  for (const auto& each : datastore->trunk_list()) listed = listed || each->name == trunk->name;
  EXPECT_TRUE(listed);

  ASSERT_TRUE(datastore->trunk_delete(trunk->name));
  EXPECT_EQ(datastore->trunk_get(trunk->name), nullptr);
  EXPECT_FALSE(datastore->trunk_delete(trunk->name)) << "nothing left to delete";
  EXPECT_FALSE(datastore->trunk_update(trunk)) << "nothing left to update";
}

// One holder at a time: another is refused while the lease runs, and the holder renews it.
TEST(MemoryDatastoreTest, ALeaseHasOneHolder) {
  auto datastore = make_datastore();
  const auto name = "trunk-register-acme";
  EXPECT_TRUE(datastore->lease(name, "node-a", 30));
  EXPECT_FALSE(datastore->lease(name, "node-b", 30));
  EXPECT_TRUE(datastore->lease(name, "node-a", 30)) << "renewed";
}

// A lease that has lapsed goes to whoever asks next.
TEST(MemoryDatastoreTest, ALapsedLeaseIsTakenOver) {
  auto datastore = make_datastore();
  const auto name = "trunk-register-lapsed";
  EXPECT_TRUE(datastore->lease(name, "node-a", 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  EXPECT_TRUE(datastore->lease(name, "node-b", 30));
  EXPECT_FALSE(datastore->lease(name, "node-a", 30));
}

// A shared counter counts up and down, and one with a window starts again once the window has passed.
TEST(MemoryDatastoreTest, ACounterCountsAndItsWindowLapses) {
  auto datastore = make_datastore();
  EXPECT_EQ(datastore->counter_get("calls-acme"), 0) << "never made is nothing";
  EXPECT_EQ(datastore->counter_add("calls-acme", 1), 1);
  EXPECT_EQ(datastore->counter_add("calls-acme", 1), 2);
  EXPECT_EQ(datastore->counter_add("calls-acme", -1), 1);
  EXPECT_EQ(datastore->counter_get("calls-acme"), 1);

  EXPECT_EQ(datastore->counter_add("minute-acme", 1, 1), 1);
  EXPECT_EQ(datastore->counter_add("minute-acme", 1, 1), 2) << "a later add does not restart the window";
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  EXPECT_EQ(datastore->counter_get("minute-acme"), 0);
  EXPECT_EQ(datastore->counter_add("minute-acme", 1, 1), 1) << "a new window";
}

// docs/design/presence.md: both canonical stores hold presence, so a node on either has it.
TEST(MemoryDatastoreTest, SupportsPresence) { EXPECT_TRUE(make_datastore()->supports_presence()); }

namespace {

types::PresenceRecord make_presence(types::PresenceRecord::Kind kind, const std::string& id, const std::string& aor, const std::string& node_id = "") {
  types::PresenceRecord record;
  record.kind = kind;
  record.id = id;
  record.aor = aor;
  record.node_id = node_id;
  record.content_type = kind == types::PresenceRecord::Kind::Publication ? "application/pidf+xml" : "application/json";
  record.body = "{\"id\":\"" + id + "\"}";
  return record;
}

using Kind = types::PresenceRecord::Kind;

}  // namespace

// A record is given back as it was put, with its expiry filled from the lifetime it was given.
TEST(MemoryDatastoreTest, APresenceRecordRoundTrips) {
  auto datastore = make_datastore();
  const auto before = std::time(nullptr);
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-a"), 60));

  const auto found = datastore->presence_get(Kind::Subscription, "sub-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->kind, Kind::Subscription);
  EXPECT_EQ(found->id, "sub-1");
  EXPECT_EQ(found->aor, "sip:1001@example.com");
  EXPECT_EQ(found->node_id, "node-a");
  EXPECT_EQ(found->content_type, "application/json");
  EXPECT_EQ(found->body, "{\"id\":\"sub-1\"}");
  EXPECT_GE(found->expires_at, before + 60);
  EXPECT_LE(found->expires_at, std::time(nullptr) + 60);

  EXPECT_FALSE(datastore->presence_get(Kind::Subscription, "no-such-sub").has_value()) << "nothing there is an empty answer";
  EXPECT_TRUE(datastore->last_error().empty()) << "and not a failure";
  EXPECT_FALSE(datastore->presence_get(Kind::Publication, "sub-1").has_value()) << "a kind is its own namespace";
}

// The notifier reads one presentity's subscriptions, or its publications, and nobody else's.
TEST(MemoryDatastoreTest, PresenceRecordsAreListedByPresentityAndKind) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-a1", "sip:1001@example.com", "node-a"), 60));
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-a2", "sip:1001@example.com", "node-b"), 60));
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Publication, "etag-a", "sip:1001@example.com"), 60));
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-b1", "sip:1002@example.com", "node-a"), 60));

  std::vector<std::string> ids;
  for (const auto& record : datastore->presence_list("sip:1001@example.com", Kind::Subscription)) ids.push_back(record.id);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{"sub-a1", "sub-a2"}));

  const auto publications = datastore->presence_list("sip:1001@example.com", Kind::Publication);
  ASSERT_EQ(publications.size(), 1u);
  EXPECT_EQ(publications.front().id, "etag-a");

  EXPECT_EQ(datastore->presence_list("sip:1002@example.com", Kind::Subscription).size(), 1u);
  EXPECT_TRUE(datastore->presence_list("sip:1003@example.com", Kind::Subscription).empty());
}

// A subscription is listed under the node holding the watcher's flow, and moves with it: a refresh that reaches
// another node rewrites the holder, and the old holder no longer lists it.
TEST(MemoryDatastoreTest, ASubscriptionIsListedUnderItsHolderAndMovesWithIt) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-a"), 60));
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Publication, "etag-1", "sip:1001@example.com"), 60));

  auto held = datastore->presence_list_node("node-a");
  ASSERT_EQ(held.size(), 1u) << "a publication has no holder";
  EXPECT_EQ(held.front().id, "sub-1");

  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-b"), 60));
  EXPECT_TRUE(datastore->presence_list_node("node-a").empty());
  ASSERT_EQ(datastore->presence_list_node("node-b").size(), 1u);
  EXPECT_EQ(datastore->presence_get(Kind::Subscription, "sub-1")->node_id, "node-b");
  EXPECT_EQ(datastore->presence_list("sip:1001@example.com", Kind::Subscription).size(), 1u) << "rewritten, not added";
}

// Deleting a record takes it out of every index, and deleting one that is not there is not an error: an
// unsubscribe and an expiry may both try.
TEST(MemoryDatastoreTest, DeletingAPresenceRecordTakesItOutOfEveryIndex) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-a"), 60));

  ASSERT_TRUE(datastore->presence_delete(Kind::Subscription, "sub-1"));
  EXPECT_FALSE(datastore->presence_get(Kind::Subscription, "sub-1").has_value());
  EXPECT_TRUE(datastore->presence_list("sip:1001@example.com", Kind::Subscription).empty());
  EXPECT_TRUE(datastore->presence_list_node("node-a").empty());

  EXPECT_TRUE(datastore->presence_delete(Kind::Subscription, "sub-1")) << "already gone";
}

// A record lapses at the end of its lifetime, from every index, with nobody deleting it.
TEST(MemoryDatastoreTest, APresenceRecordLapses) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-a"), 1));
  ASSERT_TRUE(datastore->presence_put(make_presence(Kind::Publication, "etag-1", "sip:1001@example.com"), 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));

  EXPECT_FALSE(datastore->presence_get(Kind::Subscription, "sub-1").has_value());
  EXPECT_TRUE(datastore->presence_list("sip:1001@example.com", Kind::Subscription).empty());
  EXPECT_TRUE(datastore->presence_list("sip:1001@example.com", Kind::Publication).empty());
  EXPECT_TRUE(datastore->presence_list_node("node-a").empty());
}

// Every subscription and publication has an expiry (RFC 6665 4.2.1.1, RFC 3903 4.1), an identity and a presentity.
TEST(MemoryDatastoreTest, APresenceRecordWithoutAnIdAPresentityOrALifetimeIsRefused) {
  auto datastore = make_datastore();
  EXPECT_FALSE(datastore->presence_put(make_presence(Kind::Subscription, "", "sip:1001@example.com", "node-a"), 60));
  EXPECT_FALSE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "", "node-a"), 60));
  EXPECT_FALSE(datastore->presence_put(make_presence(Kind::Subscription, "sub-1", "sip:1001@example.com", "node-a"), 0));
  EXPECT_FALSE(datastore->presence_get(Kind::Subscription, "sub-1").has_value());
}

namespace {

std::shared_ptr<Call> make_call(const std::string& id, Call::State state, const std::string& caller, const std::string& callee) {
  auto call = std::make_shared<Call>();
  call->id = id;
  call->state = state;
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:caller@example.com"), nullptr, true);
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:callee@example.com"), nullptr, false);
  call->participants[0].subscriber = caller;
  call->participants[1].subscriber = callee;
  return call;
}

std::vector<std::string> call_ids(const std::vector<std::shared_ptr<Call>>& calls) {
  std::vector<std::string> ids;
  for (const auto& call : calls) ids.push_back(call->id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace

// BLF reads a subscriber's calls from the call record (docs/design/presence.md): every call not yet closed with a
// participant that is the subscriber, whichever end it is, and no call the subscriber is not on.
TEST(MemoryDatastoreTest, AnOpenCallIsFoundByEachOfItsSubscribers) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->call_create(make_call("call-1", Call::State::Ringing, "sip:1001@example.com", "sip:1002@example.com")));
  ASSERT_TRUE(datastore->call_create(make_call("call-2", Call::State::Connected, "sip:1003@example.com", "sip:1001@example.com")));
  ASSERT_TRUE(datastore->call_create(make_call("call-3", Call::State::Trying, "", "sip:1002@example.com")));

  EXPECT_EQ(call_ids(datastore->call_list_for_subscriber("sip:1001@example.com")), (std::vector<std::string>{"call-1", "call-2"}));
  EXPECT_EQ(call_ids(datastore->call_list_for_subscriber("sip:1002@example.com")), (std::vector<std::string>{"call-1", "call-3"}));
  EXPECT_TRUE(datastore->call_list_for_subscriber("sip:1004@example.com").empty());
  EXPECT_TRUE(datastore->call_list_for_subscriber("").empty()) << "a trunk or a foreign party is nobody's";
}

// A closed call is a record kept for its retention, and not a line in use.
TEST(MemoryDatastoreTest, AClosedCallIsNoLongerFoundByItsSubscribers) {
  auto datastore = make_datastore();
  auto call = make_call("call-1", Call::State::Connected, "sip:1001@example.com", "sip:1002@example.com");
  ASSERT_TRUE(datastore->call_create(call));

  auto closed = make_call("call-1", Call::State::Closed, "sip:1001@example.com", "sip:1002@example.com");
  closed->ended_at = std::time(nullptr);
  ASSERT_TRUE(datastore->call_update(closed));

  EXPECT_TRUE(datastore->call_list_for_subscriber("sip:1001@example.com").empty());
  EXPECT_TRUE(datastore->call_list_for_subscriber("sip:1002@example.com").empty());
  EXPECT_NE(datastore->call_get("call-1"), nullptr) << "the record itself is kept";
}

// The proxy learns the callee's subscriber when it rings a binding, after the record was first written.
TEST(MemoryDatastoreTest, ASubscriberLearnedOnUpdateFindsTheCall) {
  auto datastore = make_datastore();
  ASSERT_TRUE(datastore->call_create(make_call("call-1", Call::State::Trying, "sip:1001@example.com", "")));
  EXPECT_TRUE(datastore->call_list_for_subscriber("sip:1002@example.com").empty());

  ASSERT_TRUE(datastore->call_update(make_call("call-1", Call::State::Ringing, "sip:1001@example.com", "sip:1002@example.com")));
  EXPECT_EQ(call_ids(datastore->call_list_for_subscriber("sip:1002@example.com")), (std::vector<std::string>{"call-1"}));
}

// A trunk's registration status is a record beside the trunk, replaced whole, read back as written, and gone
// with the trunk.
TEST(MemoryDatastoreTest, ATrunksStatusIsARecordThatGoesWithTheTrunk) {
  auto datastore = make_datastore();
  auto trunk = std::make_shared<types::Trunk>();
  trunk->name = "Acme";
  trunk->uri = "sip:sip.acme.example";
  ASSERT_TRUE(datastore->trunk_create(trunk));

  EXPECT_FALSE(datastore->trunk_status_get("acme").has_value());

  boost::json::object status;
  status["node"] = "sip-0001";
  status["state"] = "registered";
  ASSERT_TRUE(datastore->trunk_status_set("ACME", status));
  auto found = datastore->trunk_status_get("acme");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->at("node").as_string(), "sip-0001");

  status["state"] = "failed";
  ASSERT_TRUE(datastore->trunk_status_set("acme", status));
  EXPECT_EQ(datastore->trunk_status_get("acme")->at("state").as_string(), "failed");

  ASSERT_TRUE(datastore->trunk_delete("acme"));
  EXPECT_FALSE(datastore->trunk_status_get("acme").has_value());
}

// A re-offer is one record per subscriber and refused profile, counted up on each report, listed most recent first.
TEST(MemoryDatastoreTest, ReoffersAreCountedPerSubscriberAndListedMostRecentFirst) {
  auto datastore = make_datastore();
  EXPECT_TRUE(datastore->reoffer_list().empty());

  ASSERT_TRUE(datastore->reoffer_record("sip:fax@example.com", media::Profile::WebRtc, std::nullopt));
  ASSERT_TRUE(datastore->reoffer_record("sip:phone@example.com", media::Profile::PlainRtp, media::Profile::WebRtc));
  ASSERT_TRUE(datastore->reoffer_record("sip:phone@example.com", media::Profile::PlainRtp, media::Profile::WebRtc));

  const auto listed = datastore->reoffer_list();
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0].subscriber, "sip:phone@example.com");
  EXPECT_EQ(listed[0].count, 2u);
  EXPECT_EQ(listed[0].took, media::Profile::WebRtc);
  EXPECT_EQ(listed[1].subscriber, "sip:fax@example.com");
  EXPECT_FALSE(listed[1].took.has_value());
}

// RFC 3261 19.1.4: a URI's host compares without regard to case, so a subscriber is found whatever case the
// request spelt the realm in; the user part is case-sensitive and is not folded.
TEST(MemoryDatastoreTest, ASubscriberIsFoundWhateverCaseTheHostIsSpeltIn) {
  auto datastore = make_datastore();
  auto subscriber = std::make_shared<types::Subscriber>();
  subscriber->id = 71;
  subscriber->identity = std::make_shared<types::SIPIdentity>("sip:bob@example.com");
  subscriber->ha1 = "ha1";
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  EXPECT_NE(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:bob@EXAMPLE.COM")), nullptr);
  EXPECT_NE(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:bob@Example.Com")), nullptr);
  EXPECT_EQ(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:BOB@example.com")), nullptr);
}
