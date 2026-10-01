//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "datastores/redis_datastore.h"

#include <gtest/gtest.h>

#include <cstdlib>
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
using athenasip::datastores::RedisDatastore;

// These run against a real Redis. Point ATHENA_TEST_REDIS_URL at one to enable them;
// without it every case skips, so a machine with no Redis still runs the suite green.
//
//   redis-server --port 6399 --save '' --daemonize yes
//   ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 ctest -R RedisDatastoreTest
namespace {

std::string redis_url() {
  const char* url = std::getenv("ATHENA_TEST_REDIS_URL");
  return url ? std::string(url) : std::string();
}

// The contract is async; these tests are statements about what Redis holds, so they
// drive it through the blocking test view.
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
  realm->media.anchor = false;
  realm->media.profiles = types::MediaPolicy::Profiles::WebRtc;
  return realm;
}

std::shared_ptr<types::Account> make_account(uint64_t id, const std::string& uri) {
  auto account = std::make_shared<types::Account>();
  account->id = id;
  account->identity = std::make_shared<types::SIPIdentity>(uri);
  account->ha1 = "deadbeef";
  return account;
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

// The connection's own chatter goes through this node's logger, with a level and a scope,
// rather than straight to the console. On the console it lands in the output of
// `athenasip --add-user`, whose whole point is a clean answer.
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

  // "(Boost.Redis) " is the library's own prefix for what it prints by default.
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

  // The media policy is a realm's and has to survive being written down, or a cluster
  // would anchor differently depending on which node read the realm.
  EXPECT_FALSE(found->media.anchor);
  EXPECT_EQ(found->media.profiles, types::MediaPolicy::Profiles::WebRtc);

  auto changed = make_realm(name);
  changed->nonce_secret = "rotated";
  EXPECT_TRUE(datastore->realm_update(changed));
  EXPECT_EQ(datastore->realm_get_by_name(name)->nonce_secret, "rotated");

  EXPECT_TRUE(datastore->realm_delete(name));
  EXPECT_EQ(datastore->realm_get_by_name(name), nullptr);
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

TEST(RedisDatastoreTest, AccountRoundTripsThroughRedis) {
  REQUIRE_REDIS(datastore);
  const auto realm = "subs-" + unique_suffix() + ".example";
  const auto uri = "sip:alice@" + realm;

  auto account = make_account(4242, uri);
  ASSERT_TRUE(datastore->account_create(account));
  EXPECT_FALSE(datastore->account_create(account));

  auto found = datastore->account_get(std::make_shared<types::SIPIdentity>(uri));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, 4242u);
  EXPECT_EQ(found->ha1, "deadbeef");

  EXPECT_EQ(datastore->account_list(realm).size(), 1u);

  EXPECT_TRUE(datastore->account_delete(account->identity));
  EXPECT_EQ(datastore->account_get(std::make_shared<types::SIPIdentity>(uri)), nullptr);
  EXPECT_EQ(datastore->account_list(realm).size(), 0u);
}

TEST(RedisDatastoreTest, RegistrationsAreListedFromTheLocationIndex) {
  REQUIRE_REDIS(datastore);
  const auto realm = "loc-" + unique_suffix() + ".example";
  auto account = make_account(5150, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->account_create(account));

  auto first = std::make_shared<types::SIPUri>("sip:bob@192.0.2.10:5060");
  auto second = std::make_shared<types::SIPUri>("sip:bob@10.0.0.4:5060");

  ASSERT_TRUE(datastore->account_register(account, first, 3600, ""));
  ASSERT_TRUE(datastore->account_register(account, second, 3600, ""));

  auto locations = datastore->location_list(5150);
  ASSERT_EQ(locations.size(), 2u);
  for (const auto& location : locations) EXPECT_EQ(location.account_id, 5150u);

  ASSERT_TRUE(datastore->account_unregister(account, first));
  EXPECT_EQ(datastore->location_list(5150).size(), 1u);

  // Deleting the account takes the remaining binding with it.
  ASSERT_TRUE(datastore->account_delete(account->identity));
  EXPECT_TRUE(datastore->location_list(5150).empty());
}

// The second credential makes the round trip, and an account without one reads back as
// an account without one rather than as a broken row.
TEST(RedisDatastoreTest, TheSha256CredentialRoundTripsAndIsOptional) {
  REQUIRE_REDIS(datastore);
  const auto realm = "sha-" + unique_suffix() + ".example";

  auto both = make_account(4243, "sip:alice@" + realm);
  both->ha1_sha256 = "sha256-hash";
  ASSERT_TRUE(datastore->account_create(both));

  auto found = datastore->account_get(std::make_shared<types::SIPIdentity>("sip:alice@" + realm));
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->ha1_sha256, "sha256-hash");

  auto md5_only = make_account(4244, "sip:bob@" + realm);
  ASSERT_TRUE(datastore->account_create(md5_only));

  auto imported = datastore->account_get(std::make_shared<types::SIPIdentity>("sip:bob@" + realm));
  ASSERT_NE(imported, nullptr);
  EXPECT_TRUE(imported->ha1_sha256.empty());

  ASSERT_TRUE(datastore->account_delete(both->identity));
  ASSERT_TRUE(datastore->account_delete(md5_only->identity));
}

// A binding written by one node has to be usable by another, which is the whole reason
// the flow and the node holding it are on it (RFC 5626). They survive the round trip
// through Redis or they are of no use to the node that reads them back.
TEST(RedisDatastoreTest, RegistrationRoundTripsTheFlowAndTheNode) {
  REQUIRE_REDIS(datastore);
  const auto realm = "flow-" + unique_suffix() + ".example";
  auto account = make_account(5151, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->account_create(account));

  types::Location binding;
  binding.contact = std::make_shared<types::SIPUri>("sip:bob@192.0.2.10:5060");
  binding.path = "<sip:edge.example.com;lr>";
  binding.flow_id = "tcp://192.0.2.10:5060";
  binding.node_id = "node-a";

  ASSERT_TRUE(datastore->account_register(account, binding, 3600));

  auto locations = datastore->location_list(5151);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].path, "<sip:edge.example.com;lr>");
  EXPECT_EQ(locations[0].flow_id, "tcp://192.0.2.10:5060");
  EXPECT_EQ(locations[0].node_id, "node-a");

  ASSERT_TRUE(datastore->account_delete(account->identity));
}

// A single node writes neither, and reading back an empty flow is not the same as
// reading back the string "flow_id".
TEST(RedisDatastoreTest, RegistrationWithoutAFlowLeavesItEmpty) {
  REQUIRE_REDIS(datastore);
  const auto realm = "noflow-" + unique_suffix() + ".example";
  auto account = make_account(5152, "sip:bob@" + realm);

  ASSERT_TRUE(datastore->account_create(account));
  ASSERT_TRUE(datastore->account_register(account, std::make_shared<types::SIPUri>("sip:bob@192.0.2.11:5060"), 3600, ""));

  auto locations = datastore->location_list(5152);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_TRUE(locations[0].flow_id.empty());
  EXPECT_TRUE(locations[0].node_id.empty());

  ASSERT_TRUE(datastore->account_delete(account->identity));
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

// A multi-party call survives the round trip, participants and all.
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
  EXPECT_FALSE(datastore->account_update(make_account(1, "sip:nobody@absent-" + suffix + ".example")));

  auto call = std::make_shared<Call>();
  call->id = "absent-call-" + suffix;
  EXPECT_FALSE(datastore->call_update(call));
}

// Users and the sessions they hold, in the store the deployed node actually runs. These
// are the same statements the memory datastore is held to, deliberately: a login that
// behaves differently on memory:// and redis:// is worse than one that only works on
// one of them, and the user record is what authorises every request.

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

  // Every field, not a chosen few. A password hash a driver quietly dropped is a user
  // nobody can log in as, and roles it dropped are an authorisation failure.
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

// There is no session_update on the contract, because a token hash is 32 bytes from a
// CSPRNG and does not collide by accident. Writing the same hash again is how a session
// carries last_seen_at forward, which is what idle expiry is counted from.
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

// Issuing a session that is already dead is a caller bug, and both drivers refuse it for
// the same reason nonce_create does. Redis could not store it anyway: SETEX has no
// non-positive expiry to give it.
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

  // Twice is success twice, and so is a hash that was never held: the two drivers answer
  // the same because a logout must not tell a real token from an invented one.
  EXPECT_TRUE(datastore->session_delete("hash-a-" + suffix));
  EXPECT_TRUE(datastore->session_delete("never-existed-" + suffix));
  EXPECT_TRUE(datastore->session_delete("hash-b-" + suffix));
}

// What makes disabling a user immediate rather than eventual. The per-user index is what
// finds them, so this never needs KEYS.
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

  // Nothing to revoke is not a failure: holding none is the state the caller asked for,
  // which is what makes DELETE /users/{u}/sessions a 204 for a user who never logged in.
  EXPECT_TRUE(datastore->session_delete_for_user("judy-" + suffix));

  EXPECT_TRUE(datastore->session_delete("hash-ken-" + suffix));
}

// A live token against a user that no longer exists is a session nobody can revoke.
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

// A role this build does not know survives the round trip. It grants nothing, because
// every authorisation check asks whether a specific known role is held, and dropping it
// would mean an older node rewriting a user silently strips a role a newer node gave
// them - which is the one thing a cluster mid-upgrade must not do.
TEST(RedisDatastoreTest, ARoleThisBuildDoesNotKnowIsCarriedNotDropped) {
  REQUIRE_REDIS(datastore);
  const auto username = "future-" + unique_suffix();

  ASSERT_TRUE(datastore->user_create(make_user(username, {types::roles::manage_realms, "manage-something-later"})));

  auto found = datastore->user_get(username);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->roles.size(), 2u);
  EXPECT_TRUE(found->has_role("manage-something-later"));
  EXPECT_TRUE(found->has_role(types::roles::manage_realms));

  // And it grants nothing, because nothing asks for it.
  EXPECT_FALSE(types::roles::is_known("manage-something-later"));
  EXPECT_FALSE(found->has_role(types::roles::manage_cluster));

  EXPECT_TRUE(datastore->user_delete(username));
}
