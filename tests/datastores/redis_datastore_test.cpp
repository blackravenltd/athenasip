//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "datastores/redis_datastore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "types/password.h"
#include "types/session.h"
#include "types/trunk.h"
#include "types/url.h"
#include "types/user.h"
#include "util.h"

using namespace athenasip;
using athenasip::datastores::RedisDatastore;

// These run against a real Redis and skip without ATHENA_TEST_REDIS_URL:
//
//   redis-server --port 6399 --save '' --daemonize yes
//   ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 ./build-tests/athenasip_tests --gtest_filter='RedisDatastoreTest.*'
namespace {

std::string redis_url() {
  const char* url = std::getenv("ATHENA_TEST_REDIS_URL");
  return url ? std::string(url) : std::string();
}

// The contract is async; these tests drive it through the blocking test view.
std::shared_ptr<SyncDatastore> make_datastore() {
  const auto url = redis_url();
  if (url.empty()) return nullptr;

  auto logger = std::make_shared<MockLogger>();
  auto datastore = std::make_shared<SyncDatastore>(std::make_shared<RedisDatastore>(logger, std::make_shared<types::URL>(url)));

  if (!datastore->connect()) return nullptr;
  return datastore;
}

std::shared_ptr<types::Realm> make_realm(const std::string& name) {
  auto realm = std::make_shared<types::Realm>(name);
  realm->id = 1;
  realm->nonce_secret = "secret";
  realm->registration_timeout = 3600;
  realm->registration_minimum = 60;
  realm->behaviour.media_anchor = false;
  realm->behaviour.media_profile = types::MediaPolicy::Profiles::WebRtc;
  return realm;
}

std::shared_ptr<types::Subscriber> make_subscriber(uint64_t id, const std::string& uri) {
  auto subscriber = std::make_shared<types::Subscriber>();
  subscriber->id = id;
  subscriber->identity = std::make_shared<types::SIPIdentity>(uri);
  subscriber->ha1 = "deadbeef";
  return subscriber;
}

// Unique per run so repeated runs against the same Redis do not collide.
std::string unique_suffix() { return std::to_string(std::time(nullptr)) + "-" + std::to_string(std::rand() % 100000); }

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

#define REQUIRE_REDIS(datastore)                                                      \
  auto datastore = make_datastore();                                                  \
  if (!datastore) GTEST_SKIP() << "no Redis: set ATHENA_TEST_REDIS_URL to run these"; \
  do {                                                                                \
  } while (0)

// The connection logs through the node's logger, not the console, so `athenasip --add-user` output stays clean.
TEST(RedisDatastoreTest, TheConnectionLogsThroughTheNodesLoggerAndNotTheConsole) {
  const auto url = redis_url();
  if (url.empty()) GTEST_SKIP() << "no Redis: set ATHENA_TEST_REDIS_URL to run these";

  auto logger = std::make_shared<MockLogger>();
  auto datastore = std::make_shared<SyncDatastore>(std::make_shared<RedisDatastore>(logger, std::make_shared<types::URL>(url)));

  testing::internal::CaptureStderr();
  testing::internal::CaptureStdout();
  const bool connected = datastore->connect();
  datastore->close();
  const auto err = testing::internal::GetCapturedStderr();
  const auto out = testing::internal::GetCapturedStdout();

  ASSERT_TRUE(connected);

  // "(Boost.Redis) " is the library's default log prefix.
  EXPECT_EQ(err.find("Boost.Redis"), std::string::npos) << err;
  EXPECT_EQ(out.find("Boost.Redis"), std::string::npos) << out;

  bool heard = false;
  for (const auto& line : logger->lines()) {
    if (line.find("connection: ") != std::string::npos) heard = true;
  }
  EXPECT_TRUE(heard) << "nothing from the connection reached the node's logger";
}

TEST(RedisDatastoreTest, ConnectsAndReportsConnected) {
  REQUIRE_REDIS(datastore);

  EXPECT_TRUE(datastore->is_connected());
  EXPECT_EQ(datastore->name(), "redis");
  EXPECT_FALSE(datastore->version().empty());
}

TEST(RedisDatastoreTest, RealmRoundTripsThroughRedis) {
  REQUIRE_REDIS(datastore);
  const auto name = "realm-" + unique_suffix() + ".example";

  ASSERT_TRUE(datastore->realm_create(make_realm(name)));

  // create is not update.
  EXPECT_FALSE(datastore->realm_create(make_realm(name)));

  auto found = datastore->realm_get_by_name(name);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->nonce_secret, "secret");
  EXPECT_EQ(found->registration_timeout, 3600u);
  EXPECT_EQ(found->registration_minimum, 60u);

  // A realm's behaviour survives the store, so every node anchors the same way.
  ASSERT_TRUE(found->behaviour.media_anchor.has_value());
  EXPECT_FALSE(*found->behaviour.media_anchor);
  ASSERT_TRUE(found->behaviour.media_profile.has_value());
  EXPECT_EQ(*found->behaviour.media_profile, types::MediaPolicy::Profiles::WebRtc);

  auto changed = make_realm(name);
  changed->nonce_secret = "rotated";
  EXPECT_TRUE(datastore->realm_update(changed));
  EXPECT_EQ(datastore->realm_get_by_name(name)->nonce_secret, "rotated");

  EXPECT_TRUE(datastore->realm_delete(name));
  EXPECT_EQ(datastore->realm_get_by_name(name), nullptr);
}

// A realm is deleted with its subscribers and their bindings, and nothing of any other realm's.
TEST(RedisDatastoreTest, RealmDeleteTakesItsSubscribersAndTheirRegistrations) {
  REQUIRE_REDIS(datastore);
  const auto name = "gone-" + unique_suffix() + ".example";
  const auto other = "kept-" + unique_suffix() + ".example";

  ASSERT_TRUE(datastore->realm_create(make_realm(name)));
  ASSERT_TRUE(datastore->realm_create(make_realm(other)));

  auto alice = make_subscriber(6101, "sip:alice@" + name);
  auto carol = make_subscriber(6103, "sip:carol@" + other);
  ASSERT_TRUE(datastore->subscriber_create(alice));
  ASSERT_TRUE(datastore->subscriber_create(make_subscriber(6102, "sip:bob@" + name)));
  ASSERT_TRUE(datastore->subscriber_create(carol));
  ASSERT_TRUE(datastore->subscriber_register(alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.1:5060"), 3600, ""));
  ASSERT_TRUE(datastore->subscriber_register(carol, std::make_shared<types::SIPUri>("sip:carol@192.0.2.3:5060"), 3600, ""));

  ASSERT_TRUE(datastore->realm_delete(name));

  EXPECT_EQ(datastore->realm_get_by_name(name), nullptr);
  EXPECT_EQ(datastore->subscriber_list(name).size(), 0u);
  EXPECT_EQ(datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@" + name)), nullptr);
  EXPECT_EQ(datastore->location_list(6101).size(), 0u);

  EXPECT_EQ(datastore->subscriber_list(other).size(), 1u);
  EXPECT_EQ(datastore->location_list(6103).size(), 1u);

  datastore->realm_delete(other);
}

// What a realm did not choose stays unchosen, so it keeps following the server's default.
TEST(RedisDatastoreTest, ARealmThatChoseNoBehaviourInheritsAfterTheRoundTrip) {
  REQUIRE_REDIS(datastore);

  const auto name = "inherits-" + unique_suffix() + ".example.com";
  auto realm = std::make_shared<types::Realm>(name);
  realm->id = 2;
  realm->nonce_secret = "secret";
  ASSERT_TRUE(datastore->realm_create(realm));

  auto found = datastore->realm_get_by_name(name);
  ASSERT_NE(found, nullptr);
  EXPECT_FALSE(found->behaviour.media_anchor.has_value());
  EXPECT_FALSE(found->behaviour.media_profile.has_value());
  EXPECT_FALSE(found->behaviour.qualify_interval.has_value());

  datastore->realm_delete(name);
}

TEST(RedisDatastoreTest, ARealmsQualifyIntervalRoundTrips) {
  REQUIRE_REDIS(datastore);

  const auto name = "qualify-" + unique_suffix() + ".example.com";
  auto realm = std::make_shared<types::Realm>(name);
  realm->id = 3;
  realm->nonce_secret = "secret";
  realm->behaviour.qualify_interval = 0;
  ASSERT_TRUE(datastore->realm_create(realm));

  // Zero is a choice (never probed), not the same as unset.
  auto found = datastore->realm_get_by_name(name);
  ASSERT_NE(found, nullptr);
  ASSERT_TRUE(found->behaviour.qualify_interval.has_value());
  EXPECT_EQ(*found->behaviour.qualify_interval, 0u);

  realm->behaviour.qualify_interval = 45;
  ASSERT_TRUE(datastore->realm_update(realm));
  EXPECT_EQ(datastore->realm_get_by_name(name)->behaviour.qualify_interval, 45u);

  EXPECT_FALSE(datastore->realm_get_by_name(name)->behaviour.rewrite_contact.has_value());
  realm->behaviour.rewrite_contact = true;
  ASSERT_TRUE(datastore->realm_update(realm));
  EXPECT_EQ(datastore->realm_get_by_name(name)->behaviour.rewrite_contact, true);

  datastore->realm_delete(name);
}

TEST(RedisDatastoreTest, RealmListComesFromTheIndex) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto first = "list-a-" + suffix + ".example";
  const auto second = "list-b-" + suffix + ".example";

  const auto before = datastore->realm_list().size();

  ASSERT_TRUE(datastore->realm_create(make_realm(first)));
  ASSERT_TRUE(datastore->realm_create(make_realm(second)));

  EXPECT_EQ(datastore->realm_list().size(), before + 2);

  datastore->realm_delete(first);
  datastore->realm_delete(second);
  EXPECT_EQ(datastore->realm_list().size(), before);
}

TEST(RedisDatastoreTest, SubscriberRoundTripsThroughRedis) {
  REQUIRE_REDIS(datastore);
  const auto realm = "subs-" + unique_suffix() + ".example";
  const auto uri = "sip:alice@" + realm;

  auto subscriber = make_subscriber(4242, uri);
  ASSERT_TRUE(datastore->subscriber_create(subscriber));
  EXPECT_FALSE(datastore->subscriber_create(subscriber));

  auto found = datastore->subscriber_get(std::make_shared<types::SIPIdentity>(uri));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, 4242u);
  EXPECT_EQ(found->ha1, "deadbeef");

  EXPECT_EQ(datastore->subscriber_list(realm).size(), 1u);

  EXPECT_TRUE(datastore->subscriber_delete(subscriber->identity));
  EXPECT_EQ(datastore->subscriber_get(std::make_shared<types::SIPIdentity>(uri)), nullptr);
  EXPECT_EQ(datastore->subscriber_list(realm).size(), 0u);
}

// A subscriber's media profile survives the store, and an unset one comes back unset.
TEST(RedisDatastoreTest, ASubscribersMediaProfileRoundTrips) {
  REQUIRE_REDIS(datastore);
  const auto realm = "prof-" + unique_suffix() + ".example";

  auto chose = make_subscriber(4343, "sip:phone@" + realm);
  chose->media_profile = types::MediaPolicy::Profiles::WebRtc;
  ASSERT_TRUE(datastore->subscriber_create(chose));

  auto silent = make_subscriber(4344, "sip:desk@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(silent));

  auto found = datastore->subscriber_get(chose->identity);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->media_profile, types::MediaPolicy::Profiles::WebRtc);

  found = datastore->subscriber_get(silent->identity);
  ASSERT_NE(found, nullptr);
  EXPECT_FALSE(found->media_profile.has_value());

  datastore->subscriber_delete(chose->identity);
  datastore->subscriber_delete(silent->identity);
}

// RFC 5626: a binding's instance and reg-id survive the store, so any node knows which flow a re-registration
// replaces.
TEST(RedisDatastoreTest, AnOutboundBindingKeepsItsInstanceAndRegId) {
  REQUIRE_REDIS(datastore);
  const auto realm = "ob-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5250, "sip:carol@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:carol@192.0.2.10:5060");
  binding.instance = "<urn:uuid:00000000-0000-1000-8000-000A95A0E128>";
  binding.reg_id = 2;
  ASSERT_TRUE(datastore->subscriber_register(subscriber, binding, 3600));

  const auto found = datastore->location_list(5250);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].instance, "<urn:uuid:00000000-0000-1000-8000-000A95A0E128>");
  EXPECT_EQ(found[0].reg_id, 2u);

  datastore->subscriber_delete(subscriber->identity);
}

// RFC 8599: whether this cluster pushes to a binding, and its pn-* parameters, survive the store, so any node can
// push.
TEST(RedisDatastoreTest, APushBindingKeepsItsPushParameters) {
  REQUIRE_REDIS(datastore);
  const auto realm = "pn-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5260, "sip:carol@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:carol@192.0.2.10:5060;pn-provider=fcm;pn-param=project;pn-prid=token");
  binding.push = true;
  ASSERT_TRUE(datastore->subscriber_register(subscriber, binding, 3600));

  const auto found = datastore->location_list(5260);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_TRUE(found[0].push);
  EXPECT_EQ(found[0].contact->parameter("pn-prid"), "token");
  EXPECT_EQ(found[0].contact->parameter("pn-param"), "project");

  datastore->subscriber_delete(subscriber->identity);
}

// RFC 3261 16.5 needs every binding: fifty come back from one listing.
TEST(RedisDatastoreTest, ManyBindingsAreListedTogether) {
  REQUIRE_REDIS(datastore);
  const auto realm = "many-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5270, "sip:dave@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  for (int i = 0; i < 50; ++i) {
    types::Location binding;
    binding.contact = std::make_shared<types::SIPUri>("sip:dave@192.0.2.10:" + std::to_string(20000 + i));
    ASSERT_TRUE(datastore->subscriber_register(subscriber, binding, 3600));
  }

  EXPECT_EQ(datastore->location_list(5270).size(), 50u);

  datastore->subscriber_delete(subscriber->identity);
}

// A binding Redis has expired is not listed, though its key is still in the index.
TEST(RedisDatastoreTest, AnExpiredBindingIsNotListed) {
  REQUIRE_REDIS(datastore);
  const auto realm = "expired-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5280, "sip:erin@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  types::Location lasting;
  lasting.contact = std::make_shared<types::SIPUri>("sip:erin@192.0.2.10:5060");
  ASSERT_TRUE(datastore->subscriber_register(subscriber, lasting, 3600));

  types::Location brief;
  brief.contact = std::make_shared<types::SIPUri>("sip:erin@192.0.2.11:5060");
  ASSERT_TRUE(datastore->subscriber_register(subscriber, brief, 1));

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  const auto found = datastore->location_list(5280);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].contact->host, "192.0.2.10");

  datastore->subscriber_delete(subscriber->identity);
}

TEST(RedisDatastoreTest, RegistrationsAreListedFromTheLocationIndex) {
  REQUIRE_REDIS(datastore);
  const auto realm = "loc-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5150, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  auto first = std::make_shared<types::SIPUri>("sip:bob@192.0.2.10:5060");
  auto second = std::make_shared<types::SIPUri>("sip:bob@10.0.0.4:5060");

  ASSERT_TRUE(datastore->subscriber_register(subscriber, first, 3600, ""));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, second, 3600, ""));

  auto locations = datastore->location_list(5150);
  ASSERT_EQ(locations.size(), 2u);
  for (const auto& location : locations) EXPECT_EQ(location.subscriber_id, 5150u);

  ASSERT_TRUE(datastore->subscriber_unregister(subscriber, first));
  EXPECT_EQ(datastore->location_list(5150).size(), 1u);

  // Deleting the subscriber takes the remaining binding with it.
  ASSERT_TRUE(datastore->subscriber_delete(subscriber->identity));
  EXPECT_TRUE(datastore->location_list(5150).empty());
}

// The SHA-256 credential round-trips, and a subscriber without one reads back without one.
TEST(RedisDatastoreTest, TheSha256CredentialRoundTripsAndIsOptional) {
  REQUIRE_REDIS(datastore);
  const auto realm = "sha-" + unique_suffix() + ".example";

  auto both = make_subscriber(4243, "sip:alice@" + realm);
  both->ha1_sha256 = "sha256-hash";
  ASSERT_TRUE(datastore->subscriber_create(both));

  auto found = datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@" + realm));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1_sha256, "sha256-hash");

  auto md5_only = make_subscriber(4244, "sip:bob@" + realm);
  ASSERT_TRUE(datastore->subscriber_create(md5_only));

  auto imported = datastore->subscriber_get(std::make_shared<types::SIPIdentity>("sip:bob@" + realm));
  ASSERT_NE(imported, nullptr);
  EXPECT_TRUE(imported->ha1_sha256.empty());

  ASSERT_TRUE(datastore->subscriber_delete(both->identity));
  ASSERT_TRUE(datastore->subscriber_delete(md5_only->identity));
}

// RFC 5626: a binding's flow and the node holding it survive the store, so another node can use it.
TEST(RedisDatastoreTest, RegistrationRoundTripsTheFlowAndTheNode) {
  REQUIRE_REDIS(datastore);
  const auto realm = "flow-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5151, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->subscriber_create(subscriber));

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:bob@192.0.2.10:5060");
  binding.path = "<sip:edge.example.com;lr>";
  binding.flow_id = "tcp://192.0.2.10:5060";
  binding.node_id = "node-a";

  ASSERT_TRUE(datastore->subscriber_register(subscriber, binding, 3600));

  auto locations = datastore->location_list(5151);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].path, "<sip:edge.example.com;lr>");
  EXPECT_EQ(locations[0].flow_id, "tcp://192.0.2.10:5060");
  EXPECT_EQ(locations[0].node_id, "node-a");

  ASSERT_TRUE(datastore->subscriber_delete(subscriber->identity));
}

// A single node writes neither, and both read back empty.
TEST(RedisDatastoreTest, RegistrationWithoutAFlowLeavesItEmpty) {
  REQUIRE_REDIS(datastore);
  const auto realm = "noflow-" + unique_suffix() + ".example";
  auto subscriber = make_subscriber(5152, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->subscriber_create(subscriber));
  ASSERT_TRUE(datastore->subscriber_register(subscriber, std::make_shared<types::SIPUri>("sip:bob@192.0.2.11:5060"), 3600, ""));

  auto locations = datastore->location_list(5152);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_TRUE(locations[0].flow_id.empty());
  EXPECT_TRUE(locations[0].node_id.empty());

  ASSERT_TRUE(datastore->subscriber_delete(subscriber->identity));
}

TEST(RedisDatastoreTest, NonceRoundTripsAndIsRefusedWhenAlreadyExpired) {
  REQUIRE_REDIS(datastore);
  const auto nonce = "nonce-" + unique_suffix();
  const auto now = std::time(nullptr);

  ASSERT_TRUE(datastore->nonce_create(nonce, now + 60));
  EXPECT_TRUE(datastore->nonce_check(nonce));

  EXPECT_FALSE(datastore->nonce_create("stale-" + nonce, now - 1));
  EXPECT_FALSE(datastore->nonce_check("stale-" + nonce));
}

// A multi-party call round-trips with its participants.
TEST(RedisDatastoreTest, CallRoundTripsWithItsParticipants) {
  REQUIRE_REDIS(datastore);
  const auto id = "call-" + unique_suffix();

  auto call = std::make_shared<Call>();
  call->id = id;
  call->state = Call::State::Ringing;
  call->created_at = std::time(nullptr);
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
  call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  call->participants[0].dialog = std::make_shared<athenasip::types::Dialog>();
  call->participants[0].dialog->call_id = "call-1";
  call->participants[0].dialog->caller_tag = "alice-tag";
  call->participants[1].node_id = "sip-0002";

  ASSERT_TRUE(datastore->call_create(call));

  auto found = datastore->call_get(id);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, id);
  EXPECT_EQ(found->state, Call::State::Ringing);
  ASSERT_EQ(found->participants.size(), 2u);
  EXPECT_TRUE(found->participants[0].originator);
  EXPECT_FALSE(found->participants[1].originator);
  ASSERT_NE(found->participants[0].dialog, nullptr);
  EXPECT_EQ(found->participants[0].dialog->caller_tag, "alice-tag");
  EXPECT_EQ(found->participants[1].node_id, "sip-0002");

  call->state = Call::State::Connected;
  EXPECT_TRUE(datastore->call_update(call));
  EXPECT_EQ(datastore->call_get(id)->state, Call::State::Connected);

  EXPECT_GE(datastore->call_list().size(), 1u);
}

TEST(RedisDatastoreTest, UpdateOfSomethingAbsentFails) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();

  EXPECT_FALSE(datastore->realm_update(make_realm("absent-" + suffix + ".example")));
  EXPECT_FALSE(datastore->subscriber_update(make_subscriber(1, "sip:nobody@absent-" + suffix + ".example")));

  auto call = std::make_shared<Call>();
  call->id = "absent-call-" + suffix;
  EXPECT_FALSE(datastore->call_update(call));
}

// Users and sessions: the same requirements the memory datastore is held to, so a login behaves the same on
// both drivers.

TEST(RedisDatastoreTest, UserRoundTripsThroughRedisWithEveryField) {
  REQUIRE_REDIS(datastore);
  const auto username = "user-" + unique_suffix();

  auto user = make_user(username, {types::roles::manage_realms, types::roles::view_cluster_status});
  user->display_name = "Tom Cully";
  user->disabled = true;
  user->last_login_at = 1700000000;
  ASSERT_TRUE(datastore->user_create(user));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->username, username);
  EXPECT_EQ(found->display_name, "Tom Cully");
  EXPECT_TRUE(found->disabled);
  EXPECT_EQ(found->created_at, user->created_at);
  EXPECT_EQ(found->last_login_at, 1700000000);

  // Every field survives, password hash and roles included.
  EXPECT_EQ(found->password_hash, user->password_hash);
  EXPECT_TRUE(types::Password::verify("correct horse", found->password_hash));
  EXPECT_TRUE(found->has_role(types::roles::manage_realms));
  EXPECT_TRUE(found->has_role(types::roles::view_cluster_status));
  EXPECT_FALSE(found->has_role(types::roles::manage_admin_users));
  EXPECT_EQ(found->roles.size(), 2u);

  EXPECT_TRUE(datastore->user_delete(username));
}

TEST(RedisDatastoreTest, UserWithNoRolesRoundTrips) {
  REQUIRE_REDIS(datastore);
  const auto username = "noroles-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {})));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->roles.empty());
  EXPECT_FALSE(found->disabled);

  EXPECT_TRUE(datastore->user_delete(username));
}

TEST(RedisDatastoreTest, UsernamesAreOneNamespaceWhateverTheirCase) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto mixed = "Tom-" + suffix;

  ASSERT_TRUE(datastore->user_create(make_user(mixed, {})));

  EXPECT_NE(datastore->user_get("tom-" + suffix), nullptr);
  EXPECT_NE(datastore->user_get("TOM-" + suffix), nullptr);
  EXPECT_EQ(datastore->user_get("tom-" + suffix)->username, mixed);

  EXPECT_FALSE(datastore->user_create(make_user("TOM-" + suffix, {})));

  EXPECT_TRUE(datastore->user_delete("TOM-" + suffix));
  EXPECT_EQ(datastore->user_get(mixed), nullptr);
}

TEST(RedisDatastoreTest, UserCreateRefusesAnExistingNameAndChangesNothing) {
  REQUIRE_REDIS(datastore);
  const auto username = "taken-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {types::roles::view_cluster_status})));
  EXPECT_FALSE(datastore->user_create(make_user(username, {types::roles::manage_admin_users})));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->has_role(types::roles::view_cluster_status));
  EXPECT_FALSE(found->has_role(types::roles::manage_admin_users));

  EXPECT_TRUE(datastore->user_delete(username));
}

TEST(RedisDatastoreTest, UserUpdateRequiresAnExistingUser) {
  REQUIRE_REDIS(datastore);
  const auto username = "absent-user-" + unique_suffix();

  EXPECT_FALSE(datastore->user_update(make_user(username, {})));
  EXPECT_EQ(datastore->user_get(username), nullptr);

  ASSERT_TRUE(datastore->user_create(make_user(username, {})));

  auto changed = make_user(username, {types::roles::manage_cluster});
  changed->disabled = true;
  EXPECT_TRUE(datastore->user_update(changed));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->has_role(types::roles::manage_cluster));
  EXPECT_TRUE(found->disabled);

  EXPECT_TRUE(datastore->user_delete(username));
}

TEST(RedisDatastoreTest, UserListFindsAUserItHasJustCreated) {
  REQUIRE_REDIS(datastore);
  const auto username = "listed-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {})));

  auto users = datastore->user_list();
  bool seen = false;
  for (const auto& user : users) {
    if (user && user->username == username) seen = true;
  }
  EXPECT_TRUE(seen);

  ASSERT_TRUE(datastore->user_delete(username));

  users = datastore->user_list();
  for (const auto& user : users) {
    if (user) EXPECT_NE(user->username, username);
  }
}

TEST(RedisDatastoreTest, UserDeleteSaysWhetherThereWasOne) {
  REQUIRE_REDIS(datastore);
  const auto username = "twice-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {})));
  EXPECT_TRUE(datastore->user_delete(username));
  EXPECT_FALSE(datastore->user_delete(username));
}

TEST(RedisDatastoreTest, SessionRoundTripsByItsTokenHash) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto hash = "hash-" + suffix;

  auto session = make_session(hash, "frank-" + suffix);
  ASSERT_TRUE(datastore->session_create(session));

  auto found = datastore->session_get(hash);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->token_hash, hash);
  EXPECT_EQ(found->username, "frank-" + suffix);
  EXPECT_EQ(found->created_at, session.created_at);
  EXPECT_EQ(found->expires_at, session.expires_at);
  EXPECT_EQ(found->last_seen_at, session.last_seen_at);

  EXPECT_EQ(datastore->session_get("hash-nothing-" + suffix), nullptr);

  EXPECT_TRUE(datastore->session_delete(hash));
}

// There is no session_update: writing the same hash again carries last_seen_at forward, which idle expiry is
// counted from.
TEST(RedisDatastoreTest, WritingASessionAgainMovesItsLastSeen) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto hash = "hash-touch-" + suffix;

  auto session = make_session(hash, "grace-" + suffix);
  ASSERT_TRUE(datastore->session_create(session));

  session.last_seen_at += 300;
  ASSERT_TRUE(datastore->session_create(session));

  auto found = datastore->session_get(hash);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->last_seen_at, session.last_seen_at);
  EXPECT_EQ(found->username, "grace-" + suffix);

  EXPECT_TRUE(datastore->session_delete(hash));
}

// A session that is already expired is refused: SETEX takes no non-positive expiry.
TEST(RedisDatastoreTest, AnAlreadyExpiredSessionIsRefused) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto hash = "hash-dead-" + suffix;

  auto session = make_session(hash, "heidi-" + suffix);
  session.expires_at = std::time(nullptr) - 1;

  EXPECT_FALSE(datastore->session_create(session));
  EXPECT_EQ(datastore->session_get(hash), nullptr);
}

TEST(RedisDatastoreTest, SessionDeleteEndsThatOneSession) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto username = "ivan-" + suffix;

  ASSERT_TRUE(datastore->session_create(make_session("hash-a-" + suffix, username)));
  ASSERT_TRUE(datastore->session_create(make_session("hash-b-" + suffix, username)));

  EXPECT_TRUE(datastore->session_delete("hash-a-" + suffix));
  EXPECT_EQ(datastore->session_get("hash-a-" + suffix), nullptr);
  EXPECT_NE(datastore->session_get("hash-b-" + suffix), nullptr);

  // Deleting twice succeeds twice, as does deleting an unknown hash: a logout must not tell a real token from
  // an invented one.
  EXPECT_TRUE(datastore->session_delete("hash-a-" + suffix));
  EXPECT_TRUE(datastore->session_delete("never-existed-" + suffix));
  EXPECT_TRUE(datastore->session_delete("hash-b-" + suffix));
}

// Ends every session a user holds, found through the per-user index, never KEYS.
TEST(RedisDatastoreTest, SessionDeleteForUserEndsAllOfTheirsAndNobodyElses) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();

  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-1-" + suffix, "Judy-" + suffix)));
  ASSERT_TRUE(datastore->session_create(make_session("hash-judy-2-" + suffix, "judy-" + suffix)));
  ASSERT_TRUE(datastore->session_create(make_session("hash-ken-" + suffix, "ken-" + suffix)));

  EXPECT_TRUE(datastore->session_delete_for_user("JUDY-" + suffix));
  EXPECT_EQ(datastore->session_get("hash-judy-1-" + suffix), nullptr);
  EXPECT_EQ(datastore->session_get("hash-judy-2-" + suffix), nullptr);
  EXPECT_NE(datastore->session_get("hash-ken-" + suffix), nullptr);

  // Nothing to revoke is success, so DELETE /users/{u}/sessions is a 204 for a user who never logged in.
  EXPECT_TRUE(datastore->session_delete_for_user("judy-" + suffix));

  EXPECT_TRUE(datastore->session_delete("hash-ken-" + suffix));
}

// Deleting a user revokes their sessions.
TEST(RedisDatastoreTest, UserDeleteRevokesTheirSessions) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();
  const auto username = "erin-" + suffix;
  const auto hash = "hash-erin-" + suffix;

  ASSERT_TRUE(datastore->user_create(make_user(username, {})));
  ASSERT_TRUE(datastore->session_create(make_session(hash, username)));
  ASSERT_NE(datastore->session_get(hash), nullptr);

  EXPECT_TRUE(datastore->user_delete(username));
  EXPECT_EQ(datastore->session_get(hash), nullptr);
}

TEST(RedisDatastoreTest, UserAndSessionRefuseWhatCannotBeStored) {
  REQUIRE_REDIS(datastore);
  const auto suffix = unique_suffix();

  EXPECT_FALSE(datastore->user_create(nullptr));
  EXPECT_FALSE(datastore->user_create(make_user("", {})));
  EXPECT_FALSE(datastore->session_create(make_session("", "nobody-" + suffix)));
  EXPECT_FALSE(datastore->session_create(make_session("hash-nouser-" + suffix, "")));
  EXPECT_EQ(datastore->session_get(""), nullptr);
}

// A role this build does not know is carried, not dropped, so an older node rewriting a user mid-upgrade cannot
// strip a role a newer node granted.
TEST(RedisDatastoreTest, ARoleThisBuildDoesNotKnowIsCarriedNotDropped) {
  REQUIRE_REDIS(datastore);
  const auto username = "future-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {types::roles::manage_realms, "manage-something-later"})));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->roles.size(), 2u);
  EXPECT_TRUE(found->has_role("manage-something-later"));
  EXPECT_TRUE(found->has_role(types::roles::manage_realms));

  // It grants nothing: no check asks for it.
  EXPECT_FALSE(types::roles::is_known("manage-something-later"));
  EXPECT_FALSE(found->has_role(types::roles::manage_cluster));

  EXPECT_TRUE(datastore->user_delete(username));
}

// A trunk is stored whole and given back as stored, password and attributes included, and found by its name
// whatever its case.
TEST(RedisDatastoreTest, ATrunkRoundTrips) {
  REQUIRE_REDIS(datastore);
  auto trunk = std::make_shared<types::Trunk>();
  trunk->name = "Acme" + unique_suffix();
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
