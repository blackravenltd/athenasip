//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_add_user.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "datastores/memory_datastore.h"
#include "global_io_context.h"
#include "helpers/sync_datastore_helper.h"
#include "mocks/logger_mock.h"
#include "types/password.h"
#include "types/url.h"
#include "types/user.h"

using namespace athenasip;
using Outcome = cli::AddUserResult::Outcome;

namespace {

struct Fixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<datastores::MemoryDatastore> driver;
  std::shared_ptr<SyncDatastore> store;

  Fixture() {
    driver = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(driver);
    store->connect();
  }

  // The production iteration count is most of a second per user, and these are statements
  // about the command rather than about PBKDF2.
  static constexpr std::uint32_t kIterations = 1000;

  cli::AddUserResult add(const std::string& username, std::vector<std::string> roles = {}, const std::string& password = "a recovery password",
                         const std::string& display_name = "") {
    return cli::add_user(driver, detail::get_global_io_context().get_executor(), username, display_name, roles, password, kIterations);
  }
};

}  // namespace

TEST(CliAddUserTest, AnAdministratorIsCreatedWithoutTheApi) {
  Fixture f;

  // The way back in: no listener has started, nothing has been provisioned, and the only
  // thing this touches is the datastore.
  const auto result = f.add("tom", {types::roles::manage_realms}, "a recovery password", "Tom Cully");

  ASSERT_TRUE(result.ok()) << result.message;

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  EXPECT_EQ(user->username, "tom");
  EXPECT_EQ(user->display_name, "Tom Cully");
  EXPECT_TRUE(user->has_role(types::roles::manage_realms));
  EXPECT_FALSE(user->disabled);
}

TEST(CliAddUserTest, ThePasswordIsStoredAsAHashThatVerifies) {
  Fixture f;
  ASSERT_TRUE(f.add("tom").ok());

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);

  // What the API would have written, so the user this made can log in through it.
  EXPECT_EQ(user->password_hash.rfind("pbkdf2-sha256$", 0), 0u);
  EXPECT_TRUE(types::Password::verify("a recovery password", user->password_hash));
  EXPECT_FALSE(types::Password::verify("not it", user->password_hash));
}

TEST(CliAddUserTest, WithNoRolesGivenItCanAdministerUsers) {
  Fixture f;
  ASSERT_TRUE(f.add("tom").ok());

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);

  // A recovery user that cannot administer anybody is not a way back in, which is the
  // only reason this tool exists.
  EXPECT_TRUE(user->has_role(types::roles::manage_admin_users));
}

TEST(CliAddUserTest, AnExistingUserIsNotOverwritten) {
  Fixture f;
  ASSERT_TRUE(f.add("tom", {types::roles::manage_realms}).ok());

  const auto again = f.add("TOM", {}, "a different password");

  // Case-folded, as everywhere else. Overwriting would be a way to take over
  // a user by knowing their username and having shell access - which, granted, is most
  // of the way to owning the node anyway, but the tool should not be the easy path.
  EXPECT_EQ(again.outcome, Outcome::taken);

  auto user = f.store->user_get("tom");
  ASSERT_NE(user, nullptr);
  EXPECT_TRUE(user->has_role(types::roles::manage_realms));
  EXPECT_TRUE(types::Password::verify("a recovery password", user->password_hash));
}

TEST(CliAddUserTest, ARoleThisBuildDoesNotKnowIsRefusedAndNothingIsWritten) {
  Fixture f;

  const auto result = f.add("tom", {"manage-realms", "wizard"});

  EXPECT_EQ(result.outcome, Outcome::unknown_role);
  EXPECT_NE(result.message.find("wizard"), std::string::npos);
  EXPECT_EQ(f.store->user_get("tom"), nullptr);
}

TEST(CliAddUserTest, NothingUsableIsRefusedRatherThanGuessedAt) {
  Fixture f;

  EXPECT_EQ(f.add("").outcome, Outcome::invalid);
  EXPECT_EQ(f.add("tom", {}, "").outcome, Outcome::invalid);
  EXPECT_EQ(cli::add_user(nullptr, detail::get_global_io_context().get_executor(), "tom", "", {}, "password", 1000).outcome, Outcome::invalid);
}

TEST(CliAddUserTest, AStoreThatCannotHoldUsersSaysSoInItsOwnWords) {
  auto logger = std::make_shared<MockLogger>();

  // A datastore written against an earlier contract answers that it does not support the
  // operation rather than failing to build, and that answer has to reach the operator
  // instead of being reported as a name collision.
  class NoUsers : public datastores::MemoryDatastore {
   public:
    using MemoryDatastore::MemoryDatastore;

    void user_create(plugins::Executor on, std::shared_ptr<types::User>, plugins::StatusHandler handler) override {
      boost::asio::post(on, [handler] { handler(plugins::Status::failure("memory does not support user_create")); });
    }
  };

  auto driver = std::make_shared<NoUsers>(logger, std::make_shared<types::URL>("memory://"));
  SyncDatastore(driver).connect();

  const auto result = cli::add_user(driver, detail::get_global_io_context().get_executor(), "tom", "", {}, "a recovery password", 1000);

  EXPECT_EQ(result.outcome, Outcome::refused);
  EXPECT_NE(result.message.find("does not support"), std::string::npos);
}
