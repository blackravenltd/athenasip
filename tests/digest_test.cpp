//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "digest.h"

#include <gtest/gtest.h>

#include <string>

#include "types/authorization.h"
#include "types/subscriber.h"
#include "util.h"

using namespace athenasip;

namespace {

// RFC 2617 section 3.5: Mufasa, "Circle Of Life", in testrealm@host.com.
types::Subscriber mufasa() {
  types::Subscriber subscriber;
  subscriber.ha1 = Util::md5("Mufasa:testrealm@host.com:Circle Of Life");
  return subscriber;
}

types::Authorization credentials(const std::string& response) {
  return types::Authorization(
      "Digest username=\"Mufasa\", realm=\"testrealm@host.com\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", uri=\"/dir/index.html\", qop=auth, "
      "nc=00000001, cnonce=\"0a4f113b\", response=\"" +
      response + "\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"");
}

}  // namespace

// RFC 2617 3.5: with qop=auth the response covers nc, cnonce and qop; the RFC's own answer verifies.
TEST(DigestTest, TheRfc2617ExampleWithQopVerifies) {
  auto answered = credentials("6629fae49393a05397450978507c4ef1");

  EXPECT_EQ(digest::verify(mufasa(), answered, "GET"), "");
}

// A response computed without them does not, once qop is given.
TEST(DigestTest, AResponseThatIgnoresQopDoesNot) {
  const auto ha1 = Util::md5("Mufasa:testrealm@host.com:Circle Of Life");
  auto answered = credentials(Util::md5(ha1 + ":dcd98b7102dd2f0e8b11d0f600bfb0c093:" + Util::md5("GET:/dir/index.html")));

  EXPECT_NE(digest::verify(mufasa(), answered, "GET"), "");
}

// Only qop=auth is supported; auth-int would need the body.
TEST(DigestTest, AQopOtherThanAuthIsRefused) {
  auto answered = credentials("6629fae49393a05397450978507c4ef1");
  answered.fields["qop"] = "auth-int";

  EXPECT_NE(digest::verify(mufasa(), answered, "GET"), "");
}

// RFC 2617 3.5, from the client's side: the example's own response.
TEST(DigestTest, AnsweringTheRfc2617ExampleGivesItsResponse) {
  types::Authorization challenge(
      R"(Digest realm="testrealm@host.com", qop="auth,auth-int", nonce="dcd98b7102dd2f0e8b11d0f600bfb0c093", opaque="5ccc069c403ebaf9f0171e9517f40e41")");

  const auto answer = digest::respond(challenge, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", "0a4f113b", 1);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->fields.at("response"), "6629fae49393a05397450978507c4ef1");
  EXPECT_EQ(answer->fields.at("nc"), "00000001");
  EXPECT_EQ(answer->fields.at("qop"), "auth");
  EXPECT_EQ(answer->fields.at("opaque"), "5ccc069c403ebaf9f0171e9517f40e41");
}

// RFC 7616 3.9.1: the SHA-256 example.
TEST(DigestTest, AnsweringTheRfc7616Sha256ExampleGivesItsResponse) {
  types::Authorization challenge(
      R"(Digest realm="http-auth@example.org", qop="auth, auth-int", algorithm=SHA-256, nonce="7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v", opaque="FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS")");

  const auto answer = digest::respond(challenge, "Mufasa", "Circle of Life", "GET", "/dir/index.html", "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ", 1);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->fields.at("response"), "753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1");
  EXPECT_EQ(answer->fields.at("algorithm"), "SHA-256");
}

// What this node answers, this node's own verify accepts: the two halves agree.
TEST(DigestTest, AnAnswerWithoutQopVerifiesAgainstTheStoredCredential) {
  types::Authorization challenge(R"(Digest realm="example.com", nonce="abc123", algorithm=MD5)");
  auto answer = digest::respond(challenge, "alice", "secret", "INVITE", "sip:bob@example.com", "c", 1);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->fields.count("qop"), 0u);

  types::Subscriber alice;
  alice.ha1 = Util::md5("alice:example.com:secret");
  EXPECT_EQ(digest::verify(alice, *answer, "INVITE"), "");
}

// A challenge that only takes auth-int, which needs the body hashed, or an algorithm this node lacks, is not
// answered rather than answered wrongly.
TEST(DigestTest, AChallengeThisNodeCannotAnswerIsNotAnswered) {
  EXPECT_FALSE(digest::respond(types::Authorization(R"(Digest realm="r", nonce="n", qop="auth-int")"), "u", "p", "INVITE", "sip:x", "c", 1));
  EXPECT_FALSE(digest::respond(types::Authorization(R"(Digest realm="r", nonce="n", algorithm=SHA-512-256)"), "u", "p", "INVITE", "sip:x", "c", 1));
  EXPECT_FALSE(digest::respond(types::Authorization(R"(Basic realm="r")"), "u", "p", "INVITE", "sip:x", "c", 1));
}

// RFC 8760 2.4: SHA-256 is answered before MD5 when a server offers both.
TEST(DigestTest, SHA256IsPreferredOverMD5) {
  const auto chosen = digest::preferred(
      {types::Authorization(R"(Digest realm="r", nonce="n", algorithm=MD5)"), types::Authorization(R"(Digest realm="r", nonce="n", algorithm=SHA-256)")});
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->fields.at("algorithm"), "SHA-256");
}

// The credentials are written as RFC 3261 25.1 has them: qop and nc as tokens, the rest quoted.
TEST(DigestTest, CredentialsWriteQopAndNcAsTokens) {
  const auto answer = digest::respond(types::Authorization(R"(Digest realm="r", nonce="n", qop="auth")"), "u", "p", "INVITE", "sip:x", "c", 2);
  ASSERT_TRUE(answer);
  const auto written = answer->to_string();
  EXPECT_NE(written.find("qop=auth"), std::string::npos) << written;
  EXPECT_NE(written.find("nc=00000002"), std::string::npos) << written;
  EXPECT_NE(written.find("cnonce=\"c\""), std::string::npos) << written;
}
