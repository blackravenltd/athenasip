//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "util.h"

#include "helpers/core_fixture_helper.h"

using namespace athenasip;
using athenasip::headers::AuthorizationHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;

namespace {

// RFC 2617: HA1 is MD5(user:realm:password), and it is what the datastore stores.
const std::string kHa1 = Util::md5("alice:example.com:secret");

std::string digest_response(const std::string& ha1, const std::string& nonce, const std::string& method, const std::string& uri) {
  return Util::md5(ha1 + ":" + nonce + ":" + Util::md5(method + ":" + uri));
}

struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> connection;
  std::shared_ptr<Channel> channel;

  Fixture() {
    seed_realm("example.com");
    seed_subscriber(7, "sip:alice@example.com", kHa1);
    channel = make_channel("192.0.2.10", &connection);
  }

  std::string register_request(const std::string& authorization = "", const std::string& expires = "", const std::string& contact = "",
                               const std::string& branch = "z9hG4bK-reg", const std::string& to = "<sip:alice@example.com>") {
    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "From: " + to + ";tag=alice\r\n";
    raw += "To: " + to + "\r\n";
    raw += "Call-ID: call-registrar\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: " + (contact.empty() ? std::string("<sip:alice@192.0.2.10:5060>") : contact) + "\r\n";
    if (!expires.empty()) raw += "Expires: " + expires + "\r\n";
    if (!authorization.empty()) raw += "Authorization: " + authorization + "\r\n";
    raw += "\r\n";
    return raw;
  }

  std::string credentials(const std::string& nonce, const std::string& uri = "sip:example.com") {
    return "Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" +
           digest_response(kHa1, nonce, "REGISTER", uri) + "\"";
  }

  std::string fresh_nonce() {
    auto realm = store->realm_get_by_name("example.com");
    return mint_nonce(realm);
  }
};

}  // namespace

// RFC 3261 22.2: a REGISTER with no credentials is challenged, and the challenge has to
// carry a nonce or the client has nothing to compute with.
TEST(RegistrarTest, AnUnauthenticatedRegisterIsChallenged) {
  Fixture f;

  f.receive(f.channel, f.register_request());

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(response->header->contains("WWW-Authenticate"));

  auto challenge = response->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  ASSERT_NE(challenge, nullptr);
  EXPECT_EQ(challenge->value->type, "Digest");
  EXPECT_EQ(challenge->value->fields["realm"], "example.com");
  EXPECT_FALSE(challenge->value->fields["nonce"].empty());
}

// A nonce this node never issued, or one that has expired, is not a credential.
TEST(RegistrarTest, AnUnknownNonceIsChallenged) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials("not-a-nonce-this-node-issued")));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_EQ(f.response_with(f.connection, 200), nullptr);
}

TEST(RegistrarTest, AWrongDigestResponseIsChallenged) {
  Fixture f;

  const auto nonce = f.fresh_nonce();
  const auto wrong = "Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"sip:example.com\", response=\"" +
                     digest_response(Util::md5("alice:example.com:wrong"), nonce, "REGISTER", "sip:example.com") + "\"";

  f.receive(f.channel, f.register_request(wrong));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.3 step 5: an address of record in a domain this registrar does not serve
// is a 404, not a challenge. There is nothing here to authenticate against.
TEST(RegistrarTest, ARegisterForAnUnservedDomainIs404) {
  Fixture f;

  f.receive(f.channel, f.register_request("", "", "", "z9hG4bK-reg", "<sip:alice@elsewhere.example>"));

  EXPECT_NE(f.response_with(f.connection, 404), nullptr);
}

// A subscriber that does not exist inside a realm we do serve is challenged rather than
// refused, so a REGISTER sweep cannot tell an absent account from a wrong password.
TEST(RegistrarTest, AnUnknownSubscriberInAServedRealmIsChallenged) {
  Fixture f;

  const auto nonce = f.fresh_nonce();

  f.receive(f.channel, f.register_request(f.credentials(nonce), "", "", "z9hG4bK-reg", "<sip:nobody@example.com>"));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_EQ(f.response_with(f.connection, 404), nullptr);
}

// RFC 3261 10.3 step 7: a successful REGISTER writes the binding. Without it the
// registrar has nothing for target determination to find.
TEST(RegistrarTest, AnAuthenticatedRegisterStoresTheBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].contact->user, "alice");
  EXPECT_EQ(locations[0].contact->realm, "192.0.2.10");
}

// RFC 3261 10.3 step 8: the 200 OK lists every current binding with the time it has
// left, and carries an Expires, so a client that lost track can resynchronise from the
// response alone.
TEST(RegistrarTest, TheOkListsTheBindingsWithTheirExpiry) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  ASSERT_TRUE(response->header->contains("Expires"));
  auto expires = response->header->headers_map["Expires"][0]->as<UIntHeader>();
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->value, 600u);

  ASSERT_TRUE(response->header->contains("Contact"));
  auto contact = response->header->headers_map["Contact"][0]->as<SIPIdentityHeader>();
  ASSERT_NE(contact, nullptr);
  ASSERT_TRUE(contact->value->tags.contains("expires"));
  EXPECT_LE(std::stoul(contact->value->tags["expires"]), 600u);
  EXPECT_GT(std::stoul(contact->value->tags["expires"]), 0u);
}

// RFC 3261 10.2.1: a registrar never grants more than it is configured to, whatever the
// client asks for.
TEST(RegistrarTest, TheGrantedExpiryIsCappedByTheRealm) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "99999"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  auto expires = response->header->headers_map["Expires"][0]->as<UIntHeader>();
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->value, 3600u);
}

// RFC 3261 10.2.1.3: an expiry of zero removes the binding rather than refreshing it.
TEST(RegistrarTest, ExpiresZeroRemovesTheBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-reg-2"));

  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.2.2: a lone Contact of "*" with Expires 0 removes every binding at once.
TEST(RegistrarTest, StarContactWithExpiresZeroRemovesEveryBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.10:5060>", "z9hG4bK-reg-1"));
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.11:5060>", "z9hG4bK-reg-2"));
  ASSERT_EQ(f.store->location_list(7).size(), 2u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "*", "z9hG4bK-reg-3"));

  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.2.2: "*" with anything other than Expires 0 is malformed.
TEST(RegistrarTest, StarContactWithANonZeroExpiryIs400) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "*"));

  EXPECT_NE(f.response_with(f.connection, 400), nullptr);
}

// RFC 3261 10.2.4: a REGISTER with no Contact asks what the bindings are and must not
// change them.
TEST(RegistrarTest, ARegisterWithNoContactIsAQuery) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  std::string query = "REGISTER sip:example.com SIP/2.0\r\n";
  query += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-query\r\n";
  query += "From: <sip:alice@example.com>;tag=alice\r\n";
  query += "To: <sip:alice@example.com>\r\n";
  query += "Call-ID: call-registrar\r\n";
  query += "CSeq: 2 REGISTER\r\n";
  query += "Authorization: " + f.credentials(f.fresh_nonce()) + "\r\n";
  query += "\r\n";

  f.receive(f.channel, query);

  EXPECT_EQ(f.store->location_list(7).size(), 1u);

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);
  EXPECT_TRUE(response->header->contains("Contact"));
}

// RFC 3327: the Path the request travelled is recorded on the binding, so a later hop
// knows the route back to a contact it cannot reach directly.
TEST(RegistrarTest, PathIsRecordedOnTheBinding) {
  Fixture f;

  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-path\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: call-registrar\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
  raw += "Path: <sip:edge.example.com;lr>\r\n";
  raw += "Authorization: " + f.credentials(f.fresh_nonce()) + "\r\n";
  raw += "\r\n";

  f.receive(f.channel, raw);

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_NE(locations[0].path.find("edge.example.com"), std::string::npos);
}

// RFC 3261 17.2.2: the transaction absorbs a retransmitted REGISTER and answers it from
// what it last sent. The registrar must not see it twice, or it would write the binding
// again and issue a second challenge.
TEST(RegistrarTest, ARetransmittedRegisterIsAnsweredWithoutReachingTheRegistrarAgain) {
  Fixture f;

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("192.0.2.20", &connection);
  connection->reliable = false;

  const auto request = f.register_request("", "", "", "z9hG4bK-retransmit");

  f.receive(channel, request);
  ASSERT_EQ(f.written(connection).size(), 1u);

  f.receive(channel, request);

  auto responses = f.written(connection);
  ASSERT_EQ(responses.size(), 2u);

  // The same 401, with the same nonce: a fresh pass through the registrar would have
  // issued a new one.
  auto first = responses[0]->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  auto second = responses[1]->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->value->fields["nonce"], second->value->fields["nonce"]);
}
