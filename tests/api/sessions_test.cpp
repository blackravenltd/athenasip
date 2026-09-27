//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/sessions.h"

#include <gtest/gtest.h>

#include <ctime>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "datastores/memory_datastore.h"
#include "global_io_context.h"
#include "types/password.h"
#include "types/url.h"
#include "types/user.h"
#include "util.h"

using namespace athenasip;
using athenasip::api::Sessions;

namespace {

// What the tests say a password is. Hashed at a low iteration count because these are
// statements about sessions, not about PBKDF2, and 600000 iterations per login would
// make the file take a minute.
constexpr const char* kPassword = "correct horse battery staple";
constexpr std::uint32_t kIterations = 1000;

// A clock the test moves. Idle expiry is a rule about elapsed time and nothing else, so
// watching it against a real clock is an hour of waiting per case.
//
// It starts at the real time rather than at a round number, because the datastore prunes
// a session on the absolute expiry written on the record and does that against the real
// clock. That is the seam exactly where the contract puts it: the store decides whether
// a record has outlived its absolute expiry, and this decides everything else.
struct ManualClock {
  std::time_t now = std::time(nullptr);

  void advance(std::time_t seconds) { now += seconds; }

  Sessions::Clock fn() {
    auto* clock = this;
    return [clock] { return clock->now; };
  }
};

// A store that cannot answer, for the difference between "no" and "I could not ask".
class UnavailableDatastore : public datastores::MemoryDatastore {
 public:
  using MemoryDatastore::MemoryDatastore;

  void user_get(plugins::Executor on, std::string, plugins::Handler<std::shared_ptr<types::User>> handler) override {
    boost::asio::post(on, [handler] { handler(plugins::Result<std::shared_ptr<types::User>>::failure("the store is down")); });
  }

  void session_get(plugins::Executor on, std::string, plugins::Handler<std::shared_ptr<types::Session>> handler) override {
    boost::asio::post(on, [handler] { handler(plugins::Result<std::shared_ptr<types::Session>>::failure("the store is down")); });
  }
};

struct Fixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<datastores::MemoryDatastore> driver;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<Sessions> sessions;
  ManualClock clock;

  explicit Fixture(Sessions::Lifetimes lifetimes = {3600, 600}, std::shared_ptr<datastores::MemoryDatastore> with_driver = nullptr) {
    driver = with_driver ? with_driver : std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(driver);
    store->connect();

    sessions = std::make_shared<Sessions>(logger, driver, detail::get_global_io_context().get_executor(), lifetimes, clock.fn());
  }

  std::shared_ptr<types::User> add_user(const std::string& username, std::vector<std::string> roles, bool disabled = false) {
    auto user = std::make_shared<types::User>();
    user->username = username;
    user->roles = std::move(roles);
    user->password_hash = types::Password::hash(kPassword, kIterations);
    user->disabled = disabled;
    user->created_at = std::time(nullptr);

    EXPECT_TRUE(store->user_create(user));
    return user;
  }

  // The contract is async; a test is a statement about what came back, so it waits.
  Sessions::Login login(const std::string& username, const std::string& password) {
    std::promise<Sessions::Login> promise;
    auto future = promise.get_future();
    sessions->login(username, password, [&promise](Sessions::Login answer) { promise.set_value(std::move(answer)); });
    return future.get();
  }

  Sessions::Lookup resolve(const std::string& token) {
    std::promise<Sessions::Lookup> promise;
    auto future = promise.get_future();
    sessions->resolve(token, [&promise](Sessions::Lookup answer) { promise.set_value(std::move(answer)); });
    return future.get();
  }

  plugins::Status logout(const std::string& token) {
    std::promise<plugins::Status> promise;
    auto future = promise.get_future();
    sessions->logout(token, [&promise](plugins::Status status) { promise.set_value(std::move(status)); });
    return future.get();
  }

  // What the store holds for a token, which is what a dump of it would hand over.
  std::shared_ptr<types::Session> stored(const std::string& token) { return store->session_get(Sessions::token_hash(token)); }
};

}  // namespace

// --- Issuing ---

TEST(SessionsTest, LoginIssuesATokenAndTheRolesTheUserHolds) {
  Fixture fixture;
  fixture.add_user("tom", {types::roles::manage_realms});

  const auto answer = fixture.login("tom", kPassword);

  ASSERT_TRUE(answer.ok());
  EXPECT_FALSE(answer.value.token.empty());
  EXPECT_EQ(answer.value.roles, std::vector<std::string>{types::roles::manage_realms});
}

TEST(SessionsTest, ATokenIs32RandomBytesAndNeverTheSameTwice) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto first = fixture.login("tom", kPassword);
  const auto second = fixture.login("tom", kPassword);

  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  // 32 bytes, hex.
  EXPECT_EQ(first.value.token.size(), 64u);
  EXPECT_EQ(first.value.token.find_first_not_of("0123456789abcdef"), std::string::npos);

  EXPECT_NE(first.value.token, second.value.token);
}

TEST(SessionsTest, WhatIsStoredIsTheHashAndNotTheToken) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  // A dump of the store hands over neither the token nor anything that can be turned
  // back into one.
  EXPECT_EQ(fixture.store->session_get(answer.value.token), nullptr);

  auto held = fixture.stored(answer.value.token);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->token_hash, Util::sha256(answer.value.token));
  EXPECT_EQ(held->username, "tom");
}

TEST(SessionsTest, AbsoluteExpiryIsTheConfiguredLifetime) {
  Fixture fixture(Sessions::Lifetimes{7200, 600});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);

  ASSERT_TRUE(answer.ok());
  EXPECT_EQ(answer.value.expires_at, fixture.clock.now + 7200);
}

TEST(SessionsTest, ALoginIsRememberedOnTheUser) {
  Fixture fixture;
  fixture.add_user("tom", {});

  ASSERT_TRUE(fixture.login("tom", kPassword).ok());

  auto user = fixture.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  EXPECT_EQ(user->last_login_at, fixture.clock.now);
}

TEST(SessionsTest, AUsernameIsMatchedWhateverCaseItWasTypedIn) {
  Fixture fixture;
  fixture.add_user("Tom", {});

  const auto answer = fixture.login("TOM", kPassword);
  ASSERT_TRUE(answer.ok());

  // Filed under the key, so revoking by username finds it; the user still carries the
  // spelling it was created with.
  auto held = fixture.stored(answer.value.token);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->username, "tom");

  const auto identity = fixture.resolve(answer.value.token);
  ASSERT_TRUE(identity.ok());
  EXPECT_EQ(identity.value.user->username, "Tom");
}

// --- Refusing ---

TEST(SessionsTest, AnUnknownUserAWrongPasswordAndADisabledUserAreOneAnswer) {
  Fixture fixture;
  fixture.add_user("tom", {});
  fixture.add_user("sam", {}, /*disabled=*/true);

  const auto unknown = fixture.login("nobody", kPassword);
  const auto wrong = fixture.login("tom", "not it");
  const auto disabled = fixture.login("sam", kPassword);

  // Which of the three it was is not something the caller can tell, because between
  // them they are a list of who holds an account here.
  EXPECT_EQ(unknown.outcome, Sessions::Outcome::refused);
  EXPECT_EQ(wrong.outcome, Sessions::Outcome::refused);
  EXPECT_EQ(disabled.outcome, Sessions::Outcome::refused);

  EXPECT_TRUE(unknown.value.token.empty());
  EXPECT_TRUE(wrong.value.token.empty());
  EXPECT_TRUE(disabled.value.token.empty());
}

TEST(SessionsTest, AnEmptyUsernameOrPasswordIsRefusedWithoutAskingTheStore) {
  Fixture fixture;
  fixture.add_user("tom", {});

  EXPECT_EQ(fixture.login("", kPassword).outcome, Sessions::Outcome::refused);
  EXPECT_EQ(fixture.login("tom", "").outcome, Sessions::Outcome::refused);
}

TEST(SessionsTest, AUserWithNoRolesMayLogInAndIsToldItHasNone) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);

  ASSERT_TRUE(answer.ok());
  EXPECT_TRUE(answer.value.roles.empty());
}

TEST(SessionsTest, AStoreThatCannotBeAskedIsNotARefusal) {
  auto logger = std::make_shared<MockLogger>();
  Fixture fixture(Sessions::Lifetimes{3600, 600}, std::make_shared<UnavailableDatastore>(logger, std::make_shared<types::URL>("memory://")));

  // A 401 for a store that is down would tell an administrator to check their password.
  EXPECT_EQ(fixture.login("tom", kPassword).outcome, Sessions::Outcome::unavailable);
  EXPECT_EQ(fixture.resolve(std::string(64, 'a')).outcome, Sessions::Outcome::unavailable);
}

// --- Resolving ---

TEST(SessionsTest, ATokenResolvesToTheUserThatHoldsIt) {
  Fixture fixture;
  fixture.add_user("tom", {types::roles::view_cluster_status});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  const auto identity = fixture.resolve(answer.value.token);

  ASSERT_TRUE(identity.ok());
  ASSERT_NE(identity.value.user, nullptr);
  EXPECT_EQ(identity.value.user->key(), "tom");
  EXPECT_EQ(identity.value.expires_at, answer.value.expires_at);
}

TEST(SessionsTest, RolesAreReadOffTheUserRatherThanFrozenAtLogin) {
  Fixture fixture;
  fixture.add_user("tom", {types::roles::manage_realms});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  auto user = fixture.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->roles = {types::roles::view_cluster_status};
  ASSERT_TRUE(fixture.store->user_update(user));

  // A role removed takes effect on the next request, not at the next login.
  const auto identity = fixture.resolve(answer.value.token);
  ASSERT_TRUE(identity.ok());
  EXPECT_EQ(identity.value.user->roles, std::vector<std::string>{types::roles::view_cluster_status});
}

TEST(SessionsTest, AnUnknownTokenIsRefused) {
  Fixture fixture;
  fixture.add_user("tom", {});

  EXPECT_EQ(fixture.resolve(std::string(64, 'a')).outcome, Sessions::Outcome::refused);
  EXPECT_EQ(fixture.resolve("").outcome, Sessions::Outcome::refused);
}

TEST(SessionsTest, ASessionPastItsAbsoluteExpiryIsRefused) {
  Fixture fixture(Sessions::Lifetimes{3600, 0});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  fixture.clock.advance(3601);

  EXPECT_EQ(fixture.resolve(answer.value.token).outcome, Sessions::Outcome::refused);
}

TEST(SessionsTest, ASessionIdleTooLongIsRefusedAndGone) {
  Fixture fixture(Sessions::Lifetimes{3600, 600});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  fixture.clock.advance(601);

  EXPECT_EQ(fixture.resolve(answer.value.token).outcome, Sessions::Outcome::refused);

  // Deleted rather than left for the store, which prunes on the absolute expiry alone:
  // an idle session would otherwise sit there for the rest of its lifetime.
  EXPECT_EQ(fixture.stored(answer.value.token), nullptr);
}

TEST(SessionsTest, UsingASessionMovesTheIdleWindowAlong) {
  Fixture fixture(Sessions::Lifetimes{3600, 600});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  fixture.clock.advance(300);
  ASSERT_TRUE(fixture.resolve(answer.value.token).ok());

  // 700 seconds after the login, which is past the idle timeout measured from then, and
  // 400 after the last use, which is not.
  fixture.clock.advance(400);
  EXPECT_TRUE(fixture.resolve(answer.value.token).ok());
}

TEST(SessionsTest, TheIdleWindowIsNotRewrittenOnEveryRequest) {
  Fixture fixture(Sessions::Lifetimes{3600, 600});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  const auto issued_at = fixture.stored(answer.value.token)->last_seen_at;

  // A write per authenticated request is what this avoids: inside a tenth of the idle
  // window the record is left alone, because all the value decides is whether that
  // window has passed.
  fixture.clock.advance(30);
  ASSERT_TRUE(fixture.resolve(answer.value.token).ok());
  EXPECT_EQ(fixture.stored(answer.value.token)->last_seen_at, issued_at);

  fixture.clock.advance(31);
  ASSERT_TRUE(fixture.resolve(answer.value.token).ok());
  EXPECT_EQ(fixture.stored(answer.value.token)->last_seen_at, fixture.clock.now);
}

TEST(SessionsTest, NoIdleTimeoutMeansASessionSurvivesBeingIgnored) {
  Fixture fixture(Sessions::Lifetimes{3600, 0});
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  fixture.clock.advance(3599);

  EXPECT_TRUE(fixture.resolve(answer.value.token).ok());
}

TEST(SessionsTest, ASessionOutlivingItsUserIsNotAnIdentity) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  ASSERT_TRUE(fixture.store->user_delete("tom"));

  EXPECT_EQ(fixture.resolve(answer.value.token).outcome, Sessions::Outcome::refused);
}

TEST(SessionsTest, DisablingAUserStopsTheSessionsItAlreadyHeld) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  auto user = fixture.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  user->disabled = true;
  ASSERT_TRUE(fixture.store->user_update(user));

  // Immediate rather than eventual, which is the whole reason the roles are not carried
  // on the session.
  EXPECT_EQ(fixture.resolve(answer.value.token).outcome, Sessions::Outcome::refused);
}

// --- Ending ---

TEST(SessionsTest, LogoutEndsTheSessionItWasGiven) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto answer = fixture.login("tom", kPassword);
  ASSERT_TRUE(answer.ok());

  EXPECT_TRUE(fixture.logout(answer.value.token).ok);

  EXPECT_EQ(fixture.stored(answer.value.token), nullptr);
  EXPECT_EQ(fixture.resolve(answer.value.token).outcome, Sessions::Outcome::refused);
}

TEST(SessionsTest, LogoutOfATokenThatNamesNothingIsSuccess) {
  Fixture fixture;

  // The state the caller asked for is that the session is gone, and it is.
  EXPECT_TRUE(fixture.logout("").ok);
}

TEST(SessionsTest, LoggingOneSessionOutLeavesTheOthersAlone) {
  Fixture fixture;
  fixture.add_user("tom", {});

  const auto first = fixture.login("tom", kPassword);
  const auto second = fixture.login("tom", kPassword);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  ASSERT_TRUE(fixture.logout(first.value.token).ok);

  EXPECT_EQ(fixture.resolve(first.value.token).outcome, Sessions::Outcome::refused);
  EXPECT_TRUE(fixture.resolve(second.value.token).ok());
}
