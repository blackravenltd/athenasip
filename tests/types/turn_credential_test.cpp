//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/turn_credential.h"

#include <gtest/gtest.h>

#include <string>

using athenasip::types::TurnCredential;

namespace {

constexpr std::time_t kNow = 1790000000;

}  // namespace

// The scheme, from coturn's own README for use-auth-secret: the username is the expiry,
// optionally with a name after a colon, and the password is base64(HMAC-SHA1(secret,
// username)).
//
// This value was computed independently of this code - `printf '1790003600' | openssl
// dgst -sha1 -hmac 'a shared secret' -binary | base64` - because a test that computed the
// expected answer the same way the code does would agree with the code about anything,
// including being wrong. The TURN server is what has to agree, and it is not in this
// repository.
TEST(TurnCredentialTest, TheCredentialIsWhatCoturnWillRecompute) {
  const auto credential = TurnCredential::issue("a shared secret", "", kNow, 3600);

  EXPECT_EQ(credential.username, "1790003600");
  EXPECT_EQ(credential.password, "5jBk9nG898I9gAD2asWqE6PFJKc=");
  EXPECT_EQ(credential.expires_at, 1790003600);
}

TEST(TurnCredentialTest, TheUsernameCarriesTheExpiryAndThenTheName) {
  const auto credential = TurnCredential::issue("a shared secret", "tom", kNow, 3600);

  // coturn does not look at the name. It is there so a relay session can be tied back to
  // whoever asked for it in a log.
  EXPECT_EQ(credential.username, "1790003600:tom");
  EXPECT_EQ(credential.password, "Z6NBxs82TjUSc4t5ruZ2mhlym6A=");

  // And it is inside the MAC, so it cannot be edited on the way past.
  EXPECT_NE(credential.password, TurnCredential::issue("a shared secret", "", kNow, 3600).password);
}

TEST(TurnCredentialTest, TheTtlIsWhatDecidesTheExpiry) {
  EXPECT_EQ(TurnCredential::issue("s", "", kNow, 60).expires_at, kNow + 60);
  EXPECT_EQ(TurnCredential::issue("s", "", kNow, 86400).expires_at, kNow + 86400);
}

TEST(TurnCredentialTest, ADifferentSecretIsADifferentCredential) {
  const auto one = TurnCredential::issue("secret one", "", kNow, 3600);
  const auto two = TurnCredential::issue("secret two", "", kNow, 3600);

  // Same username, because that is only the expiry. The whole of the difference is in the
  // MAC, which is the point: the secret is the only thing a TURN server is told.
  EXPECT_EQ(one.username, two.username);
  EXPECT_NE(one.password, two.password);
}

TEST(TurnCredentialTest, NoSecretIsNoCredentialRatherThanABrokenOne) {
  const auto credential = TurnCredential::issue("", "tom", kNow, 3600);

  // A deployment with no TURN server has nothing to authenticate to. Handing out a
  // credential computed under an empty secret would be handing out one that cannot work.
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

  // HMAC-SHA1 is 20 bytes, which is 28 base64 characters with one pad. Not the URL-safe
  // alphabet: this travels in a JSON body and in an RTCIceServer, never in a path.
  EXPECT_EQ(credential.password.size(), 28u);
  EXPECT_EQ(credential.password.back(), '=');
  EXPECT_EQ(credential.password.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="), std::string::npos);
}
