//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/password.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using athenasip::types::Password;

namespace {

// The shipped cost is 600,000 iterations, about a second a hash. Format and behaviour do not depend on the
// count, so these tests use a cheap one.
constexpr std::uint32_t kCheap = 1000;

}  // namespace

// The only test that asserts the shipped cost.
TEST(PasswordTest, TheDefaultCostIsHighEnoughToBeWorthPaying) { EXPECT_GE(Password::default_iterations, 600000u); }

// What is stored does not contain the password.
TEST(PasswordTest, TheStoredFormDoesNotContainThePassword) {
  const auto stored = Password::hash("correct horse battery staple", kCheap);

  EXPECT_EQ(stored.find("correct horse"), std::string::npos);
  EXPECT_EQ(stored.find("battery"), std::string::npos);
}

// The stored form is self-describing, so the algorithm and cost can change per user.
TEST(PasswordTest, TheStoredFormSaysHowItWasMade) {
  const auto stored = Password::hash("hunter2", kCheap);

  EXPECT_EQ(stored.rfind("pbkdf2-sha256$", 0), 0u) << stored;

  // algorithm, iterations, salt, hash
  std::size_t fields = 1;
  for (const auto character : stored) {
    if (character == '$') ++fields;
  }
  EXPECT_EQ(fields, 4u) << stored;
}

TEST(PasswordTest, ThePasswordItWasMadeFromVerifies) {
  const auto stored = Password::hash("hunter2", kCheap);

  EXPECT_TRUE(Password::verify("hunter2", stored));
  EXPECT_FALSE(Password::verify("hunter3", stored));
  EXPECT_FALSE(Password::verify("", stored));
  EXPECT_FALSE(Password::verify("HUNTER2", stored));
}

// A per-user salt: the same password never hashes the same way twice.
TEST(PasswordTest, TheSamePasswordHashesDifferentlyEveryTime) {
  std::set<std::string> seen;

  for (int i = 0; i < 16; ++i) seen.insert(Password::hash("the same password", kCheap));

  EXPECT_EQ(seen.size(), 16u);

  // Each still verifies, because the salt is stored with the hash.
  for (const auto& stored : seen) EXPECT_TRUE(Password::verify("the same password", stored));
}

// Anything that is not a hash this code wrote is refused, so a corrupted or emptied record is never a way in.
TEST(PasswordTest, NothingElseVerifiesAgainstAnything) {
  EXPECT_FALSE(Password::verify("hunter2", ""));
  EXPECT_FALSE(Password::verify("hunter2", "hunter2"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$600000$onlythree"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$notanumber$c2FsdA==$aGFzaA=="));
  EXPECT_FALSE(Password::verify("hunter2", "argon2id$1$c2FsdA==$aGFzaA=="));

  // Zero iterations is refused.
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$0$c2FsdA==$aGFzaA=="));
}

// The count is stored with the hash, so a record made at another cost still verifies.
TEST(PasswordTest, AHashMadeAtADifferentCostStillVerifies) {
  const auto cheap = Password::hash("hunter2", 1000);

  EXPECT_NE(cheap.find("$1000$"), std::string::npos) << cheap;
  EXPECT_TRUE(Password::verify("hunter2", cheap));
  EXPECT_FALSE(Password::verify("wrong", cheap));
}

// An empty password is refused at the point it would be stored.
TEST(PasswordTest, AnEmptyPasswordCannotBeStored) {
  EXPECT_TRUE(Password::hash("", kCheap).empty());
  EXPECT_FALSE(Password::verify("", Password::hash("", kCheap)));
}

// A stored hash of any length but 32 bytes is refused. PBKDF2 output for a shorter length is a prefix of the
// longer, so verifying to the stored length would let a hash truncated to one byte match about one password
// in 256, turning write access to the datastore into a login.
TEST(PasswordTest, ATruncatedStoredHashVerifiesNothing) {
  const auto stored = Password::hash("correct horse", 1000);
  ASSERT_FALSE(stored.empty());

  const auto last_dollar = stored.rfind('$');
  ASSERT_NE(last_dollar, std::string::npos);

  const auto prefix = stored.substr(0, last_dollar + 1);
  const auto encoded = stored.substr(last_dollar + 1);

  // "cA==" is one base64 quantum: a single decoded byte.
  for (const std::size_t quanta : {1u, 2u, 4u}) {
    const auto truncated = prefix + encoded.substr(0, quanta * 4);

    // Not even the right password verifies against a malformed record.
    EXPECT_FALSE(Password::verify("correct horse", truncated)) << "truncated to " << quanta << " quanta";
    EXPECT_FALSE(Password::verify("anything else", truncated)) << "truncated to " << quanta << " quanta";
  }
}

TEST(PasswordTest, AStoredHashLongerThanThisCodeWritesVerifiesNothing) {
  const auto stored = Password::hash("correct horse", 1000);

  // Longer is refused too: this code writes exactly 32 bytes.
  const auto last_dollar = stored.rfind('$');
  const auto padded = stored.substr(0, last_dollar + 1) + "AAAA" + stored.substr(last_dollar + 1);

  EXPECT_FALSE(Password::verify("correct horse", padded));
}
