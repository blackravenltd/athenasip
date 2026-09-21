//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "datastores/redis_datastore.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "types/url.h"

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

}  // namespace

#define REQUIRE_REDIS(datastore)                                                      \
  auto datastore = make_datastore();                                                  \
  if (!datastore) GTEST_SKIP() << "no Redis: set ATHENA_TEST_REDIS_URL to run these"; \
  do {                                                                                \
  } while (0)

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
