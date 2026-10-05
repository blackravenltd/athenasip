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
