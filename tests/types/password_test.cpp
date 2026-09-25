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

// The real cost is 600,000 iterations, which is the point of it and takes about a
// second a hash. What these tests are about is the format and the behaviour, neither of
// which changes with the count, so they buy one at a time they can afford. The default
// being what it should be is asserted once, without computing anything.
constexpr std::uint32_t kCheap = 1000;

}  // namespace

// The cost is a real cost, and it is not a test's business to make it cheap for
// everybody. This is the only test that asserts the shipped number.
TEST(PasswordTest, TheDefaultCostIsHighEnoughToBeWorthPaying) { EXPECT_GE(Password::default_iterations, 600000u); }

// The whole point: a password is never stored, and what is stored cannot be turned back
// into it. Everything else here is in service of that.
TEST(PasswordTest, TheStoredFormDoesNotContainThePassword) {
  const auto stored = Password::hash("correct horse battery staple", kCheap);

  EXPECT_EQ(stored.find("correct horse"), std::string::npos);
  EXPECT_EQ(stored.find("battery"), std::string::npos);
}

// Self-describing, so the algorithm and the cost can change per user later without
// invalidating everybody who has not logged in since.
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

// A per-user salt, or the same password twice gives the same hash and one cracked
// password is every account that shares it.
TEST(PasswordTest, TheSamePasswordHashesDifferentlyEveryTime) {
  std::set<std::string> seen;

  for (int i = 0; i < 16; ++i) seen.insert(Password::hash("the same password", kCheap));

  EXPECT_EQ(seen.size(), 16u);

  // And every one of them still verifies, which is what the salt being stored with the
  // hash is for.
  for (const auto& stored : seen) EXPECT_TRUE(Password::verify("the same password", stored));
}

// Anything that is not a hash this code wrote is refused rather than interpreted. A
// verifier that returned true for a malformed record would turn a corrupted row, or a
// column somebody had emptied, into a way in.
TEST(PasswordTest, NothingElseVerifiesAgainstAnything) {
  EXPECT_FALSE(Password::verify("hunter2", ""));
  EXPECT_FALSE(Password::verify("hunter2", "hunter2"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$600000$onlythree"));
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$notanumber$c2FsdA==$aGFzaA=="));
  EXPECT_FALSE(Password::verify("hunter2", "argon2id$1$c2FsdA==$aGFzaA=="));

  // Zero iterations is not a cost, it is a hash nobody computed.
  EXPECT_FALSE(Password::verify("hunter2", "pbkdf2-sha256$0$c2FsdA==$aGFzaA=="));
}

// A stored record from an older cost still verifies, which is the point of keeping the
// count with the hash rather than in a constant.
TEST(PasswordTest, AHashMadeAtADifferentCostStillVerifies) {
  const auto cheap = Password::hash("hunter2", 1000);

  EXPECT_NE(cheap.find("$1000$"), std::string::npos) << cheap;
  EXPECT_TRUE(Password::verify("hunter2", cheap));
  EXPECT_FALSE(Password::verify("wrong", cheap));
}

// An empty password is a decision, not an accident: refused at the point it would be
// stored, so no account can exist that anything logs into with nothing.
TEST(PasswordTest, AnEmptyPasswordCannotBeStored) {
  EXPECT_TRUE(Password::hash("", kCheap).empty());
  EXPECT_FALSE(Password::verify("", Password::hash("", kCheap)));
}
