//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/turn_credential.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using athenasip::types::TurnCredential;

namespace {

constexpr std::time_t kNow = 1790000000;

}  // namespace

// coturn use-auth-secret: the username is the expiry, optionally with ":name", and the password is
// base64(HMAC-SHA1(secret, username)). The expected value was computed independently of this code:
//   printf '1790003600' | openssl dgst -sha1 -hmac 'a shared secret' -binary | base64
TEST(TurnCredentialTest, TheCredentialIsWhatCoturnWillRecompute) {
  const auto credential = TurnCredential::issue("a shared secret", "", kNow, 3600);

  EXPECT_EQ(credential.username, "1790003600");
  EXPECT_EQ(credential.password, "5jBk9nG898I9gAD2asWqE6PFJKc=");
  EXPECT_EQ(credential.expires_at, 1790003600);
}

TEST(TurnCredentialTest, TheUsernameCarriesTheExpiryAndThenTheName) {
  const auto credential = TurnCredential::issue("a shared secret", "tom", kNow, 3600);

  // coturn ignores the name; it ties a relay session to its requester in a log.
  EXPECT_EQ(credential.username, "1790003600:tom");
  EXPECT_EQ(credential.password, "Z6NBxs82TjUSc4t5ruZ2mhlym6A=");

  // The name is inside the MAC, so it cannot be altered.
  EXPECT_NE(credential.password, TurnCredential::issue("a shared secret", "", kNow, 3600).password);
}

TEST(TurnCredentialTest, TheTtlIsWhatDecidesTheExpiry) {
  EXPECT_EQ(TurnCredential::issue("s", "", kNow, 60).expires_at, kNow + 60);
  EXPECT_EQ(TurnCredential::issue("s", "", kNow, 86400).expires_at, kNow + 86400);
}

TEST(TurnCredentialTest, ADifferentSecretIsADifferentCredential) {
  const auto one = TurnCredential::issue("secret one", "", kNow, 3600);
  const auto two = TurnCredential::issue("secret two", "", kNow, 3600);

  // The username is only the expiry; the whole difference is in the MAC.
  EXPECT_EQ(one.username, two.username);
  EXPECT_NE(one.password, two.password);
}

TEST(TurnCredentialTest, NoSecretIsNoCredentialRatherThanABrokenOne) {
  const auto credential = TurnCredential::issue("", "tom", kNow, 3600);

  // With no secret there is no TURN server to authenticate to, so no credential is issued.
  EXPECT_TRUE(credential.username.empty());
  EXPECT_TRUE(credential.password.empty());
  EXPECT_EQ(credential.expires_at, 0);
}

TEST(TurnCredentialTest, ACredentialThatHasAlreadyExpiredIsNotIssued) {
  const auto credential = TurnCredential::issue("a shared secret", "", kNow, 0);

  EXPECT_TRUE(credential.username.empty());
  EXPECT_TRUE(credential.password.empty());
}

TEST(TurnCredentialTest, ThePasswordIsStandardBase64) {
  const auto credential = TurnCredential::issue("a shared secret", "", kNow, 3600);

  // HMAC-SHA1 is 20 bytes: 28 base64 characters with one pad, in the standard alphabet, not the URL-safe one.
  EXPECT_EQ(credential.password.size(), 28u);
  EXPECT_EQ(credential.password.back(), '=');
  EXPECT_EQ(credential.password.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="), std::string::npos);
}

// coturn parses the username as <expiry>[:<name>] and refuses the credential when the name is not one it
// accepts, such as one with a space. A client cannot tell that from a bad secret.
TEST(TurnCredentialTest, ANameThatWouldBreakTheUsernameIsFiltered) {
  const auto credential = TurnCredential::issue("a shared secret", "a configuration token", kNow, 3600);

  // No spaces, and still exactly one colon: coturn splits on the first one.
  EXPECT_EQ(credential.username, "1790003600:aconfigurationtoken");
  EXPECT_EQ(credential.username.find(' '), std::string::npos);
  EXPECT_EQ(std::count(credential.username.begin(), credential.username.end(), ':'), 1);
}

TEST(TurnCredentialTest, ANameWithNothingUsableInItIsOmittedEntirely) {
  const auto credential = TurnCredential::issue("a shared secret", "   ", kNow, 3600);

  // A colon with nothing after it would be a malformed username, so an empty name is omitted.
  EXPECT_EQ(credential.username, "1790003600");
}

TEST(TurnCredentialTest, ANameIsBoundedSoTheUsernameStaysReasonable) {
  const auto credential = TurnCredential::issue("a shared secret", std::string(200, 'x'), kNow, 3600);

  EXPECT_EQ(credential.username, "1790003600:" + std::string(32, 'x'));
}

TEST(TurnCredentialTest, AColonInTheNameCannotSplitTheUsernameAgain) {
  const auto credential = TurnCredential::issue("a shared secret", "tom:admin", kNow, 3600);

  // A colon in the name would make the expiry ambiguous and could smuggle in a second field.
  EXPECT_EQ(credential.username, "1790003600:tomadmin");
}
