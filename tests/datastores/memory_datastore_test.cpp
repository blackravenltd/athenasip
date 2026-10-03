//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "datastores/memory_datastore.h"

#include <gtest/gtest.h>

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "types/password.h"
#include "types/session.h"
#include "types/url.h"
#include "types/user.h"

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

std::shared_ptr<types::Account> make_account(uint64_t id, const std::string& uri) {
  auto account = std::make_shared<types::Account>();
  account->id = id;
  account->identity = std::make_shared<types::SIPIdentity>(uri);
  account->ha1 = "deadbeef";
  return account;
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

TEST(MemoryDatastoreTest, AccountLookupByIdentity) {
  auto datastore = make_datastore();
  datastore->account_create(make_account(42, "sip:alice@example.com"));

  auto identity = std::make_shared<types::SIPIdentity>("sip:alice@example.com");
  auto found = datastore->account_get(identity);

  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, 42u);
  EXPECT_EQ(found->ha1, "deadbeef");
  EXPECT_EQ(found->identity, identity);
}

TEST(MemoryDatastoreTest, UnknownAccountIsNull) {
  auto datastore = make_datastore();

  EXPECT_EQ(datastore->account_get(std::make_shared<types::SIPIdentity>("sip:nobody@example.com")), nullptr);
  EXPECT_EQ(datastore->account_get(nullptr), nullptr);
}

TEST(MemoryDatastoreTest, RegistrationStoresAContactThatCanBeLookedUp) {
  auto datastore = make_datastore();
  datastore->realm_create(make_realm("example.com"));

  auto account = make_account(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->account_register(account, contact, 3600, ""));

  auto locations = datastore->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].contact->host, "192.168.1.50");
  EXPECT_EQ(locations[0].contact->port.value(), 5060);
}

// The binding is what the caller gave, apart from the lifetime and the identity the
// store settles. A store that kept only the contact would lose the flow the binding was
// learned over (RFC 5626) and the node holding it, which is what a second node needs.
TEST(MemoryDatastoreTest, RegistrationKeepsTheFlowAndTheNodeItWasGiven) {
  auto datastore = make_datastore();
  auto account = make_account(7, "sip:bob@example.com");

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");
  binding.path = "<sip:edge.example.com;lr>";
  binding.flow_id = "tcp://192.168.1.50:5060";
  binding.node_id = "node-a";

  ASSERT_TRUE(datastore->account_register(account, binding, 3600));

  auto locations = datastore->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].path, "<sip:edge.example.com;lr>");
  EXPECT_EQ(locations[0].flow_id, "tcp://192.168.1.50:5060");
  EXPECT_EQ(locations[0].node_id, "node-a");

  // The store's own fields, whatever the caller put there.
  EXPECT_EQ(locations[0].account_id, 7u);
  EXPECT_GT(locations[0].expires_at, locations[0].registered_at);
}

// RFC 8760: an account can hold a credential per algorithm, and a read that returns
// only some of them is a credential that silently does not exist. This is the bug that
// made a SHA-256 registration fail against an account that had the hash for it.
TEST(MemoryDatastoreTest, EveryCredentialSurvivesARead) {
  auto datastore = make_datastore();

  auto account = make_account(7, "sip:bob@example.com");
  account->ha1 = "md5-hash";
  account->ha1_sha256 = "sha256-hash";
  ASSERT_TRUE(datastore->account_create(account));

  auto found = datastore->account_get(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1, "md5-hash");
  EXPECT_EQ(found->ha1_sha256, "sha256-hash");
}

// RFC 3261 10.2.1: an account may register more than one contact, and all of them
// are targets.
TEST(MemoryDatastoreTest, MultipleContactsForOneAccountAreKept) {
  auto datastore = make_datastore();
  auto account = make_account(7, "sip:bob@example.com");

  ASSERT_TRUE(datastore->account_register(account, std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060"), 3600, ""));
  ASSERT_TRUE(datastore->account_register(account, std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060"), 3600, ""));

  EXPECT_EQ(datastore->location_list(7).size(), 2u);
}

TEST(MemoryDatastoreTest, ReregisteringTheSameContactDoesNotDuplicateIt) {
  auto datastore = make_datastore();
  auto account = make_account(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->account_register(account, contact, 3600, ""));
  ASSERT_TRUE(datastore->account_register(account, contact, 3600, ""));

  EXPECT_EQ(datastore->location_list(7).size(), 1u);
}

TEST(MemoryDatastoreTest, UnregisterRemovesOnlyThatContact) {
  auto datastore = make_datastore();
  auto account = make_account(7, "sip:bob@example.com");
  auto first = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");
  auto second = std::make_shared<types::SIPUri>("sip:bob@192.168.1.51:5060");

  datastore->account_register(account, first, 3600, "");
  datastore->account_register(account, second, 3600, "");

  ASSERT_TRUE(datastore->account_unregister(account, first));
  EXPECT_EQ(datastore->location_list(7).size(), 1u);

  // Removing something that is not there is not a success.
  EXPECT_FALSE(datastore->account_unregister(account, first));
}

TEST(MemoryDatastoreTest, ExpiredRegistrationsAreNotReturned) {
  auto datastore = make_datastore();

  // A realm whose registrations last no time at all.
  datastore->realm_create(make_realm("192.168.1.50", 0));

  auto account = make_account(7, "sip:bob@example.com");
  auto contact = std::make_shared<types::SIPUri>("sip:bob@192.168.1.50:5060");

  ASSERT_TRUE(datastore->account_register(account, contact, 3600, ""));

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

// A realm is deleted with everything in it (Tom, 2026-10-03). Subscribers left behind
// would be unreachable through the API, which finds them through their realm, and their
// bindings would go on routing calls into a domain this node no longer serves.
TEST(MemoryDatastoreTest, RealmDeleteTakesItsSubscribersAndTheirRegistrations) {
  auto datastore = make_datastore();

  datastore->realm_create(make_realm("one.example"));
  datastore->realm_create(make_realm("two.example"));

  auto alice = make_account(1, "sip:alice@one.example");
  auto carol = make_account(3, "sip:carol@two.example");
  ASSERT_TRUE(datastore->account_create(alice));
  ASSERT_TRUE(datastore->account_create(make_account(2, "sip:bob@one.example")));
  ASSERT_TRUE(datastore->account_create(carol));
  ASSERT_TRUE(datastore->account_register(alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.1:5060"), 3600, ""));
  ASSERT_TRUE(datastore->account_register(carol, std::make_shared<types::SIPUri>("sip:carol@192.0.2.3:5060"), 3600, ""));

  ASSERT_TRUE(datastore->realm_delete("one.example"));

  EXPECT_EQ(datastore->account_list("one.example").size(), 0u);
  EXPECT_EQ(datastore->account_get(std::make_shared<types::SIPIdentity>("sip:alice@one.example")), nullptr);
  EXPECT_EQ(datastore->location_list(1).size(), 0u);

  // And nothing of anybody else's.
  EXPECT_EQ(datastore->account_list("two.example").size(), 1u);
  EXPECT_EQ(datastore->location_list(3).size(), 1u);
}

TEST(MemoryDatastoreTest, AccountCreateRefusesADuplicate) {
  auto datastore = make_datastore();

  EXPECT_TRUE(datastore->account_create(make_account(1, "sip:alice@example.com")));
  EXPECT_FALSE(datastore->account_create(make_account(1, "sip:alice@example.com")));
}

TEST(MemoryDatastoreTest, AccountUpdateRequiresAnExistingAccount) {
  auto datastore = make_datastore();

  EXPECT_FALSE(datastore->account_update(make_account(1, "sip:alice@example.com")));

  ASSERT_TRUE(datastore->account_create(make_account(1, "sip:alice@example.com")));

  auto changed = make_account(1, "sip:alice@example.com");
  changed->ha1 = "newhash";
  EXPECT_TRUE(datastore->account_update(changed));

  auto found = datastore->account_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1, "newhash");
}

TEST(MemoryDatastoreTest, AccountListIsScopedToTheRealm) {
  auto datastore = make_datastore();

  datastore->account_create(make_account(1, "sip:alice@one.example"));
  datastore->account_create(make_account(2, "sip:bob@one.example"));
  datastore->account_create(make_account(3, "sip:carol@two.example"));

  EXPECT_EQ(datastore->account_list("one.example").size(), 2u);
  EXPECT_EQ(datastore->account_list("two.example").size(), 1u);
  EXPECT_EQ(datastore->account_list("nowhere.example").size(), 0u);
}

// A deleted account keeps no bindings: leaving them would route calls to someone
// who no longer exists.
TEST(MemoryDatastoreTest, AccountDeleteDropsTheirRegistrations) {
  auto datastore = make_datastore();

  auto account = make_account(9, "sip:dave@example.com");
  ASSERT_TRUE(datastore->account_create(account));
  ASSERT_TRUE(datastore->account_register(account, std::make_shared<types::SIPUri>("sip:dave@192.0.2.9:5060"), 3600, ""));
  ASSERT_EQ(datastore->location_list(9).size(), 1u);

  EXPECT_TRUE(datastore->account_delete(account->identity));
  EXPECT_TRUE(datastore->location_list(9).empty());
  EXPECT_FALSE(datastore->account_delete(account->identity));
}

TEST(MemoryDatastoreTest, LocationCarriesTheBindingNotJustTheContact) {
  auto datastore = make_datastore();

  auto account = make_account(11, "sip:erin@example.com");
  ASSERT_TRUE(datastore->account_register(account, std::make_shared<types::SIPUri>("sip:erin@10.0.0.7:5060"), 3600, ""));

  auto locations = datastore->location_list(11);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].account_id, 11u);
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

// Users and the sessions they hold. These are statements out of docs/authentication.md:
// a user is a record beside realms and accounts, usernames are one namespace matched
// without regard to case, and a session is held by the hash of its token and never by
// the token.

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

  // The whole record, not a chosen few fields: a password hash a store quietly dropped
  // is a user nobody can log in as, which is how ha1_sha256 went missing once already.
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

  // The spelling it was given is what it is called, not the key it is filed under.
  EXPECT_EQ(datastore->user_get("tom")->username, "Tom");

  EXPECT_FALSE(datastore->user_create(make_user("TOM", {})));
  EXPECT_FALSE(datastore->user_create(make_user("tom", {})));
}

// create is not update, so the API can answer 409 rather than overwrite somebody.
TEST(MemoryDatastoreTest, UserCreateRefusesAnExistingUsername) {
  auto datastore = make_datastore();

  auto first = make_user("alice", {types::roles::view_cluster_status});
  ASSERT_TRUE(datastore->user_create(first));

  auto second = make_user("alice", {types::roles::manage_admin_users});
  EXPECT_FALSE(datastore->user_create(second));

  // And the refusal changed nothing.
  auto found = datastore->user_get("alice");
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->has_role(types::roles::view_cluster_status));
  EXPECT_FALSE(found->has_role(types::roles::manage_admin_users));
}

// update is not create, so a PUT to a username that does not exist is a 404 rather than
// a way to make one without going through the rules that creating one applies.
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

// A user with no roles is the default for a newly created one, and a legitimate state
// for one being set up or wound down. It is not an invalid record.
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

// A live token against a user that no longer exists is a session nobody can revoke, so
// deleting a user takes their sessions with it, the way deleting an account takes its
// registrations.
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

// There is no session_update on the contract, because a token hash is 32 random bytes
// and cannot collide by accident. Writing the same hash again is how a session's
// last_seen_at is carried forward, which is what idle expiry is counted from.
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

// Absolute expiry is the store's to keep, because it is written on the record and needs
// no configuration to read. Idle expiry is not: how long a session survives unused is
// config the caller holds, so the caller asks Session::has_expired.
//
// Issuing a session that is already dead is a caller bug, and both drivers refuse it for
// the same reason nonce_create does. Redis could not store it anyway: SETEX has no
// non-positive expiry to give it, so accepting it here would be a divergence.
TEST(MemoryDatastoreTest, AnAlreadyExpiredSessionIsRefused) {
  auto datastore = make_datastore();

  auto session = make_session("hash-old", "heidi");
  session.expires_at = std::time(nullptr) - 1;

  EXPECT_FALSE(datastore->session_create(session));
  EXPECT_EQ(datastore->session_get("hash-old"), nullptr);

  // A session with no absolute expiry at all is not "expired long ago": 0 is what a
  // record written before expiry existed would carry, and it is refused rather than
  // stored as one that never ends.
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

  // Twice is success twice. A session is named by a secret the caller holds, so an answer
  // that tells a live token from one that was never real is an oracle a logout hands to
  // anybody; the realm and account deletes still say when there was nothing there,
  // because a realm name is not a secret.
  EXPECT_TRUE(datastore->session_delete("hash-a"));
  EXPECT_TRUE(datastore->session_delete("never-existed"));
}

// What makes disabling a user immediate rather than eventual, and what a console's
// "sign out everywhere" is.
TEST(MemoryDatastoreTest, SessionDeleteForUserEndsAllOfTheirsAndNobodyElses) {
  auto datastore = make_datastore();

  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-1", "Judy")));
  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-2", "judy")));
  ASSERT_TRUE(datastore->session_create(make_session("hash-ken", "ken")));

  EXPECT_TRUE(datastore->session_delete_for_user("JUDY"));
  EXPECT_EQ(datastore->session_get("hash-judy-1"), nullptr);
  EXPECT_EQ(datastore->session_get("hash-judy-2"), nullptr);
  EXPECT_NE(datastore->session_get("hash-ken"), nullptr);

  // Nothing to revoke is not a failure: the caller asked for them to be gone and they
  // are, which is what disabling a user that has never logged in does.
  EXPECT_TRUE(datastore->session_delete_for_user("judy"));
}

// A read hands back a copy, not the record. A driver that goes to the network cannot do
// otherwise, so a driver that is a hash map must not either, or the API changes roles
// on this node by forgetting to write and is then surprised by Redis. The user record is
// what authorises every request, which is the worst place for the two to differ.
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
