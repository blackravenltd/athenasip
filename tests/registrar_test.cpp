//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/json.hpp>

#include "types/trunk.h"

#include <memory>
#include <string>

#include <ctime>

#include "digest.h"
#include "events/topics.h"
#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "helpers/core_fixture_helper.h"
#include "util.h"

using namespace athenasip;
using athenasip::headers::AuthorizationHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;

namespace {

// RFC 2617: HA1 is MD5(user:realm:password), as the datastore stores it.
const std::string kHa1 = Util::md5("alice:example.com:secret");

// RFC 8760: the same credential under SHA-256.
const std::string kHa1Sha256 = Util::sha256("alice:example.com:secret");

std::string digest_response(const std::string& ha1, const std::string& nonce, const std::string& method, const std::string& uri) {
  return Util::md5(ha1 + ":" + nonce + ":" + Util::md5(method + ":" + uri));
}

std::string digest_response_sha256(const std::string& ha1, const std::string& nonce, const std::string& method, const std::string& uri) {
  return Util::sha256(ha1 + ":" + nonce + ":" + Util::sha256(method + ":" + uri));
}

struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> connection;
  std::shared_ptr<Channel> channel;

  Fixture() {
    seed_realm("example.com");
    seed_subscriber(7, "sip:alice@example.com", kHa1);
    channel = make_channel("192.0.2.10", &connection);
  }

  // Each REGISTER the next CSeq under the one Call-ID, as a client sends them (RFC 3261 10.2.4).
  std::uint64_t cseq = 0;

  std::string register_request(const std::string& authorization = "", const std::string& expires = "", const std::string& contact = "",
                               const std::string& branch = "z9hG4bK-reg", const std::string& to = "<sip:alice@example.com>") {
    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "From: " + to + ";tag=alice\r\n";
    raw += "To: " + to + "\r\n";
    raw += "Call-ID: call-registrar\r\n";
    raw += "CSeq: " + std::to_string(++cseq) + " REGISTER\r\n";
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

  // The credentials a client sends in answer to the SHA-256 challenge.
  std::string credentials_sha256(const std::string& nonce, const std::string& uri = "sip:example.com") {
    return "Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", algorithm=SHA-256, response=\"" +
           digest_response_sha256(kHa1Sha256, nonce, "REGISTER", uri) + "\"";
  }

  std::string fresh_nonce() {
    auto realm = store->realm_get_by_name("example.com");
    return mint_nonce(realm);
  }

  // The registrar minimum of RFC 3261 10.3 step 7. A new realm has none.
  void set_registration_minimum(std::uint32_t minimum) {
    auto realm = store->realm_get_by_name("example.com");
    realm->registration_minimum = minimum;
    store->realm_update(realm);
  }
};

}  // namespace

// RFC 3261 22.2: a REGISTER without credentials is challenged, and the challenge
// carries a nonce.
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

// RFC 8760 2.1: one challenge per algorithm, SHA-256 first, each in a field of its own. MD5 is the last: JsSIP keeps
// only a header's last field and answers MD5 alone, so AthenaPhone and the console depend on the order.
TEST(RegistrarTest, TheChallengesAreSha256ThenMd5AsSeparateFields) {
  Fixture f;

  f.receive(f.channel, f.register_request());

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  const auto& challenges = response->header->headers_map["WWW-Authenticate"];
  ASSERT_EQ(challenges.size(), 2u);
  EXPECT_EQ(challenges[0]->as<AuthorizationHeader>()->value->fields["algorithm"], "SHA-256");
  EXPECT_EQ(challenges[1]->as<AuthorizationHeader>()->value->fields["algorithm"], "MD5");
}

// A nonce this node did not issue, or an expired one, is challenged again.
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


// The challenge for a nonce this node did not mint is plain: stale is for an answer that was right over a nonce
// that had expired (RFC 7616 3.3), and a forgery was not.
TEST(RegistrarTest, AForgedNonceIsNotStale) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials("not-a-nonce-this-node-issued")));

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  auto challenge = response->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  ASSERT_NE(challenge, nullptr);
  EXPECT_FALSE(challenge->value->contains_field("stale"));
}

// A right answer over a nonce older than the realm's nonce_expiry is challenged with stale=true, and the binding
// is not made.
TEST(RegistrarTest, AnExpiredNonceIsChallengedAsStale) {
  Fixture f;

  const auto realm = f.store->realm_get_by_name("example.com");
  const auto expired = digest::nonce_mint(*realm, "192.0.2.10", std::time(nullptr) - static_cast<std::time_t>(realm->nonce_expiry) - 1);
  f.receive(f.channel, f.register_request(f.credentials(expired)));

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  auto challenge = response->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  ASSERT_NE(challenge, nullptr);
  EXPECT_EQ(challenge->value->fields["stale"], "true");
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 7616 5.6: an answer without qop spends its nonce. The same Authorization sent again is a replay: challenged
// as stale, so an honest client signs a fresh nonce, and a captured one is worth one REGISTER at most.
TEST(RegistrarTest, AReplayedAnswerIsChallengedAsStale) {
  Fixture f;

  const auto authorization = f.credentials(f.fresh_nonce());
  f.receive(f.channel, f.register_request(authorization, "600"));
  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  f.receive(f.channel, f.register_request(authorization, "0", "", "z9hG4bK-reg-2"));

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  auto challenge = response->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  ASSERT_NE(challenge, nullptr);
  EXPECT_EQ(challenge->value->fields["stale"], "true");
  // The replayed REGISTER with Expires 0 removed nothing.
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// RFC 7616 3.4.3: with qop=auth one nonce answers many requests, each with a higher nc. The count that does not
// rise is a replay.
TEST(RegistrarTest, WithQopTheCountMustRiseUnderOneNonce) {
  Fixture f;

  const auto nonce = f.fresh_nonce();
  const auto with_count = [&nonce](const std::string& nc) {
    const auto response = Util::md5(kHa1 + ":" + nonce + ":" + nc + ":cn:auth:" + Util::md5("REGISTER:sip:example.com"));
    return "Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"sip:example.com\", qop=auth, nc=" + nc +
           ", cnonce=\"cn\", response=\"" + response + "\"";
  };

  f.receive(f.channel, f.register_request(with_count("00000001"), "600", "", "z9hG4bK-reg-1"));
  f.receive(f.channel, f.register_request(with_count("00000002"), "600", "", "z9hG4bK-reg-2"));
  EXPECT_EQ(f.response_with(f.connection, 401), nullptr);
  EXPECT_EQ(f.responses_with(f.connection, 200).size(), 2u);

  f.receive(f.channel, f.register_request(with_count("00000002"), "600", "", "z9hG4bK-reg-3"));
  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>()->value->fields["stale"], "true");
}

// RFC 3261 22.4 (5): the uri in the credentials is the Request-URI. An answer that signs another is challenged,
// so an Authorization captured from one request cannot be put on a different one.
TEST(RegistrarTest, ADigestUriThatIsNotTheRequestUriIsChallenged) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce(), "sip:other.example")));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}
// RFC 3261 10.3 step 5: an address of record in a domain this registrar does not serve
// is a 404, not a challenge.
TEST(RegistrarTest, ARegisterForAnUnservedDomainIs404) {
  Fixture f;

  f.receive(f.channel, f.register_request("", "", "", "z9hG4bK-reg", "<sip:alice@elsewhere.example>"));

  EXPECT_NE(f.response_with(f.connection, 404), nullptr);
}

// An unknown subscriber in a served realm is challenged, not refused, so a REGISTER
// sweep cannot tell an absent subscriber from a wrong password.
TEST(RegistrarTest, AnUnknownSubscriberInAServedRealmIsChallenged) {
  Fixture f;

  const auto nonce = f.fresh_nonce();

  f.receive(f.channel, f.register_request(f.credentials(nonce), "", "", "z9hG4bK-reg", "<sip:nobody@example.com>"));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_EQ(f.response_with(f.connection, 404), nullptr);
}

// RFC 3261 10.3 step 7: a successful REGISTER stores the binding.
TEST(RegistrarTest, AnAuthenticatedRegisterStoresTheBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u);
  EXPECT_EQ(locations[0].contact->user, "alice");
  EXPECT_EQ(locations[0].contact->host, "192.0.2.10");
}

// RFC 3261 10.3 step 8: the 200 lists every current binding with its remaining expiry.
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

// RFC 3261 10.2.1: the granted expiry is capped by the realm's maximum.
TEST(RegistrarTest, TheGrantedExpiryIsCappedByTheRealm) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "99999"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  auto expires = response->header->headers_map["Expires"][0]->as<UIntHeader>();
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->value, 3600u);
}

// RFC 3261 10.2.1.3: an expiry of zero removes the binding.
TEST(RegistrarTest, ExpiresZeroRemovesTheBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-reg-2"));

  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.2.2: a lone Contact of "*" with Expires 0 removes every binding.
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

// RFC 3261 10.2.4: a REGISTER with no Contact is a query and changes nothing.
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

// RFC 3327: the Path the request carried is recorded on the binding.
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

// RFC 8760 2.1: one challenge per algorithm, most preferred first: SHA-256, then MD5.
TEST(RegistrarTest, TheChallengeOffersSha256ThenMd5) {
  Fixture f;

  f.receive(f.channel, f.register_request());

  auto response = f.response_with(f.connection, 401);
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(response->header->contains("WWW-Authenticate"));

  const auto& challenges = response->header->headers_map["WWW-Authenticate"];
  ASSERT_EQ(challenges.size(), 2u);

  auto first = challenges[0]->as<AuthorizationHeader>();
  auto second = challenges[1]->as<AuthorizationHeader>();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  EXPECT_EQ(Util::to_upper(first->value->fields["algorithm"]), "SHA-256");
  EXPECT_EQ(Util::to_upper(second->value->fields["algorithm"]), "MD5");

  // Both carry a nonce.
  EXPECT_FALSE(first->value->fields["nonce"].empty());
  EXPECT_FALSE(second->value->fields["nonce"].empty());
}

// RFC 3261 25.1: algorithm is a token, not a quoted string.
TEST(RegistrarTest, TheAlgorithmIsSentAsATokenNotAQuotedString) {
  Fixture f;

  f.receive(f.channel, f.register_request());

  ASSERT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_NE(f.connection->written.find("algorithm=SHA-256"), std::string::npos);
  EXPECT_EQ(f.connection->written.find("algorithm=\"SHA-256\""), std::string::npos);
}

// A subscriber with a SHA-256 credential authenticates with it.
TEST(RegistrarTest, ASha256ResponseAuthenticates) {
  Fixture f;

  // Provisioned from a password, so it has both credentials.
  auto subscriber = f.store->subscriber_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(subscriber, nullptr);
  subscriber->ha1_sha256 = kHa1Sha256;
  ASSERT_TRUE(f.store->subscriber_update(subscriber));

  f.receive(f.channel, f.register_request(f.credentials_sha256(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// A subscriber imported as a bare MD5 hash has no SHA-256 credential; a SHA-256 response
// from it is challenged again, not accepted or answered 500.
TEST(RegistrarTest, ASha256ResponseFromAnMd5OnlySubscriberIsChallenged) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials_sha256(f.fresh_nonce())));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// An MD5 response authenticates, with or without the algorithm parameter.
TEST(RegistrarTest, AnMd5ResponseStillAuthenticates) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// RFC 3608: the 200 carries a Service-Route naming this node, which is where the client
// sends its later requests.
TEST(RegistrarTest, TheOkCarriesAServiceRouteForThisNode) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  ASSERT_TRUE(response->header->contains("Service-Route"));

  auto route = response->header->headers_map["Service-Route"][0]->as<SIPIdentityHeader>();
  ASSERT_NE(route, nullptr);
  ASSERT_NE(route->value, nullptr);
  ASSERT_NE(route->value->uri, nullptr);

  // The node as the flow reached it, with lr so the next hop does not rewrite the
  // Request-URI.
  EXPECT_EQ(route->value->uri->host, "192.0.2.1");
  EXPECT_TRUE(route->value->uri->has_parameter("lr"));
}

// A listener bound to the wildcard has local address 0.0.0.0, so the Service-Route uses
// the advertised address.
TEST(RegistrarTest, TheServiceRouteUsesTheAdvertisedAddress) {
  Fixture f;
  f.config->sip_public_address = "203.0.113.5";

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(response->header->contains("Service-Route"));

  auto route = response->header->headers_map["Service-Route"][0]->as<SIPIdentityHeader>();
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->value->uri->host, "203.0.113.5");
}

// RFC 3261 17.2.2: the transaction answers a retransmitted REGISTER from what it last
// sent; the registrar does not see it twice.
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

  // The same 401 with the same nonce; the registrar would have issued a new one.
  auto first = responses[0]->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();
  auto second = responses[1]->header->headers_map["WWW-Authenticate"][0]->as<AuthorizationHeader>();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->value->fields["nonce"], second->value->fields["nonce"]);
}

// RFC 3261 10.3 step 7: an interval below the realm minimum is refused with 423 and a
// Min-Expires stating that minimum.
TEST(RegistrarTest, AnIntervalBelowTheRealmMinimumIsRefusedWithMinExpires) {
  Fixture f;
  f.set_registration_minimum(120);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "30"));

  auto response = f.response_with(f.connection, 423);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_message, "Interval Too Brief");

  ASSERT_TRUE(response->header->contains("Min-Expires"));
  auto minimum = response->header->headers_map["Min-Expires"][0]->as<UIntHeader>();
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->value, 120u);

  // The remaining steps are skipped: no binding is written.
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.3 step 7: a Contact expires parameter is read before the Expires header.
TEST(RegistrarTest, AContactExpiresParameterBelowTheMinimumIsRefusedToo) {
  Fixture f;
  f.set_registration_minimum(120);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.10:5060>;expires=30"));

  EXPECT_NE(f.response_with(f.connection, 423), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 3261 10.3 step 7: only an interval under one hour can be refused as too brief.
TEST(RegistrarTest, AnIntervalOfAnHourIsNeverTooBrief) {
  Fixture f;
  f.set_registration_minimum(7200);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "3600"));

  EXPECT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// RFC 3261 10.2.1.3: zero is a removal and is never refused as too brief.
TEST(RegistrarTest, AZeroExpiryIsNeverTooBrief) {
  Fixture f;
  f.set_registration_minimum(120);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-reg-2"));

  EXPECT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// A realm with no minimum grants whatever interval is asked for.
TEST(RegistrarTest, WithNoMinimumABriefRegistrationIsGranted) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "30"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  auto expires = response->header->headers_map["Expires"][0]->as<UIntHeader>();
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->value, 30u);
}

// RFC 3261 10.3 step 6: with no Contact, step 7 is skipped, so a query's Expires is not
// checked against the minimum.
TEST(RegistrarTest, AQueryIsNotRefusedAsTooBrief) {
  Fixture f;
  f.set_registration_minimum(120);

  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-query\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: call-registrar\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Expires: 30\r\n";
  raw += "Authorization: " + f.credentials(f.fresh_nonce()) + "\r\n";
  raw += "\r\n";

  f.receive(f.channel, raw);

  EXPECT_NE(f.response_with(f.connection, 200), nullptr);
}

// --- RFC 5626 outbound (section 6) ---

namespace {

const std::string kInstance = "\"<urn:uuid:00000000-0000-1000-8000-000A95A0E128>\"";

std::string outbound_contact(const std::string& address, int reg_id) {
  return "<sip:alice@" + address + ">;+sip.instance=" + kInstance + ";reg-id=" + std::to_string(reg_id);
}

std::string with_header(std::string raw, const std::string& header) {
  raw.insert(raw.size() - 2, header + "\r\n");
  return raw;
}

}  // namespace

// RFC 5626 6: an outbound registration is acknowledged with Require: outbound, and the
// binding keeps the instance and reg-id that identify it.
TEST(RegistrarTest, AnOutboundRegistrationIsHonouredAndSaidToBe) {
  Fixture f;

  f.receive(f.channel,
            with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)), "Supported: outbound, path"));

  auto ok = f.response_with(f.connection, 200);
  ASSERT_NE(ok, nullptr);
  ASSERT_TRUE(ok->header->contains("Require"));
  EXPECT_NE(ok->header->headers_map["Require"][0]->to_string().find("outbound"), std::string::npos);

  // The Contact listed back carries them too.
  const auto listed = ok->header->headers_map["Contact"][0]->to_string();
  EXPECT_NE(listed.find("reg-id=1"), std::string::npos) << listed;
  EXPECT_NE(listed.find("+sip.instance"), std::string::npos) << listed;

  const auto bindings = f.store->location_list(7);
  ASSERT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0].instance, "<urn:uuid:00000000-0000-1000-8000-000A95A0E128>");
  EXPECT_EQ(bindings[0].reg_id, 1u);
}

// RFC 5626 6: a binding is identified by instance and reg-id, not Contact. Registering
// the same pair again, from a new flow or with a new Contact, replaces it.
TEST(RegistrarTest, AnOutboundFlowRegisteredAgainReplacesTheOldOne) {
  Fixture f;

  f.receive(f.channel, with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)), "Supported: outbound"));
  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  std::shared_ptr<MockConnection> second_connection;
  auto second = f.make_channel("192.0.2.10", &second_connection, "udp", 5070);
  f.receive(second, with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5070", 1), "z9hG4bK-reg-2"),
                                "Supported: outbound"));
  ASSERT_NE(f.response_with(second_connection, 200), nullptr);

  const auto bindings = f.store->location_list(7);
  ASSERT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0].contact->port, 5070);
  EXPECT_EQ(bindings[0].flow_id, second->flow_id());
}

// A second reg-id is a second flow from the same client.
TEST(RegistrarTest, ASecondRegIdIsASecondFlow) {
  Fixture f;

  f.receive(f.channel, with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)), "Supported: outbound"));
  f.receive(f.channel, with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5061", 2), "z9hG4bK-reg-2"),
                                   "Supported: outbound"));

  EXPECT_EQ(f.store->location_list(7).size(), 2u);
}

// expires=0 removes the flow by instance and reg-id, whatever the Contact says.
TEST(RegistrarTest, AnOutboundFlowIsRemovedByItsIdentity) {
  Fixture f;

  f.receive(f.channel, with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)), "Supported: outbound"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, with_header(f.register_request(f.credentials(f.fresh_nonce()), "0", outbound_contact("192.0.2.10:5099", 1), "z9hG4bK-reg-2"),
                                   "Supported: outbound"));
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 5626 6: without Supported: outbound a reg-id is ignored: no Require in the answer
// and an ordinary binding.
TEST(RegistrarTest, ARegIdWithoutSupportedOutboundIsAnOrdinaryBinding) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)));

  auto ok = f.response_with(f.connection, 200);
  ASSERT_NE(ok, nullptr);
  EXPECT_FALSE(ok->header->contains("Require"));

  const auto bindings = f.store->location_list(7);
  ASSERT_EQ(bindings.size(), 1u);
  EXPECT_EQ(bindings[0].reg_id, 0u);
}

// RFC 5626 6: an outbound registration through an edge proxy whose Path lacks "ob" is
// refused with 439 First Hop Lacks Outbound Support.
TEST(RegistrarTest, AnOutboundRegistrationThroughAnEdgeWithoutObIsRefused) {
  Fixture f;

  auto request = with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1)), "Supported: outbound, path");
  request = with_header(request, "Path: <sip:edge.example.com;lr>");
  f.receive(f.channel, request);

  EXPECT_NE(f.response_with(f.connection, 439), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());

  // Through an edge that says ob, it is accepted.
  auto through_ob = with_header(f.register_request(f.credentials(f.fresh_nonce()), "", outbound_contact("192.0.2.10:5060", 1), "z9hG4bK-reg-ob"),
                                "Supported: outbound, path");
  through_ob = with_header(through_ob, "Path: <sip:edge.example.com;lr;ob>");
  f.receive(f.channel, through_ob);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// A conference URI is a room, not a subscriber: a REGISTER for one is refused before any challenge
// (docs/design/conferencing.md). Conferencing is the standard scripts' (decision 2), so this is proved under them.
TEST(RegistrarTest, ARegisterForAConferenceUriIsRefused) {
  if (!CoreFixture::lua_policy()) GTEST_SKIP() << "conferencing is athenasip.standard's; run with ATHENA_TEST_POLICY=lua";
  Fixture f;
  auto rooms = std::make_shared<types::Trunk>();
  rooms->name = "rooms";
  rooms->uri = "sip:192.0.2.40:5060";
  rooms->attributes = boost::json::parse(R"({"focus": {"prefix": "conf-"}})").as_object();
  ASSERT_TRUE(f.store->trunk_create(rooms));

  f.receive(f.channel, f.register_request("", "", "", "z9hG4bK-room", "<sip:conf-sales@example.com>"));

  EXPECT_NE(Fixture::response_with(f.connection, 403), nullptr);
  EXPECT_EQ(Fixture::response_with(f.connection, 401), nullptr) << "refused before any challenge";
}

// --- Guessing (review finding 24, AuthGuard) ---

namespace {

std::string wrong_credentials(const std::string& nonce, const std::string& user = "alice") {
  return "Digest username=\"" + user + "\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"sip:example.com\", response=\"" +
         digest_response(Util::md5(user + ":example.com:wrong"), nonce, "REGISTER", "sip:example.com") + "\"";
}

}  // namespace

// An address that keeps answering wrongly is banned: past sip.auth_limits.failures_source it is not answered at all,
// and each failure is published for a watcher, the one that banned it as well.
TEST(RegistrarTest, AnAddressThatKeepsGuessingIsBannedAndThenNotAnswered) {
  Fixture f;
  f.config->sip_auth_failures_source = {3, 1};
  f.core->auth_limits_reload();

  std::vector<std::string> failures;
  std::vector<std::string> bans;
  f.bus->subscribe(events::topics::auth_failed(), [&failures](const std::string&, const std::string& message) { failures.push_back(message); });
  f.bus->subscribe(events::topics::source_banned(), [&bans](const std::string&, const std::string& message) { bans.push_back(message); });

  for (int i = 0; i < 4; ++i) {
    f.receive(f.channel, f.register_request(wrong_credentials(f.fresh_nonce()), "", "", "z9hG4bK-guess-" + std::to_string(i)));
  }
  EXPECT_EQ(f.responses_with(f.connection, 401).size(), 4u);
  ASSERT_EQ(failures.size(), 4u);
  EXPECT_NE(failures[0].find("\"source\":\"192.0.2.10\""), std::string::npos) << failures[0];
  EXPECT_NE(failures[0].find("\"aor\":\"sip:alice@example.com\""), std::string::npos) << failures[0];
  ASSERT_EQ(bans.size(), 1u);
  EXPECT_NE(bans[0].find("\"source\":\"192.0.2.10\""), std::string::npos) << bans[0];

  // Banned: even the right password gets nothing.
  const auto before = Fixture::written(f.connection).size();
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "", "z9hG4bK-after-ban"));
  EXPECT_EQ(Fixture::written(f.connection).size(), before);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// A guess spread over many addresses closes the address of record to strangers, answered 403 with Retry-After,
// while an address that registered alice lately goes on registering her.
TEST(RegistrarTest, AnAorGuessedAtFromManyAddressesIsClosedToStrangersButNotItsOwnPhone) {
  Fixture f;
  f.config->sip_auth_failures_aor = {3, 1};
  f.core->auth_limits_reload();

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  for (int i = 0; i < 4; ++i) {
    const auto address = "203.0.113." + std::to_string(i + 1);
    std::shared_ptr<MockConnection> connection;
    auto guesser = f.make_channel(address, &connection);
    f.receive(guesser, f.register_request(wrong_credentials(f.mint_nonce(f.store->realm_get_by_name("example.com"), address)), "", "",
                                          "z9hG4bK-g-" + std::to_string(i)));
  }

  std::shared_ptr<MockConnection> stranger_connection;
  auto stranger = f.make_channel("203.0.113.50", &stranger_connection);
  f.receive(stranger, f.register_request(wrong_credentials(f.mint_nonce(f.store->realm_get_by_name("example.com"), "203.0.113.50")), "", "",
                                         "z9hG4bK-stranger"));
  auto refused = Fixture::response_with(stranger_connection, 403);
  ASSERT_NE(refused, nullptr);
  EXPECT_TRUE(refused->header->contains("Retry-After"));

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "", "z9hG4bK-reg-again"));
  EXPECT_EQ(f.responses_with(f.connection, 200).size(), 2u) << "alice's own phone is not shut out";
}

// A nonce is bound to the address it was sent to, so one fetched from elsewhere and answered from a spoofed
// source is a nonce this realm did not mint for it: challenged, never counted as a guess against that source.
TEST(RegistrarTest, ANonceFromAnotherAddressIsChallengedAndCountsAgainstNobody) {
  Fixture f;
  f.config->sip_auth_failures_source = {1, 1};
  f.core->auth_limits_reload();

  for (int i = 0; i < 5; ++i) {
    const auto elsewhere = f.mint_nonce(f.store->realm_get_by_name("example.com"), "203.0.113.66");
    f.receive(f.channel, f.register_request(wrong_credentials(elsewhere), "", "", "z9hG4bK-spoof-" + std::to_string(i)));
  }
  EXPECT_EQ(f.responses_with(f.connection, 401).size(), 5u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "", "z9hG4bK-real"));
  EXPECT_NE(f.response_with(f.connection, 200), nullptr) << "not banned by guesses it never made";
}

// Challenges to one address are limited: past sip.auth_limits.challenges_source a request needing one is 403.
TEST(RegistrarTest, ChallengesToOneAddressAreLimited) {
  Fixture f;
  f.config->sip_auth_challenges_source = {2, 1};
  f.core->auth_limits_reload();

  for (int i = 0; i < 3; ++i) f.receive(f.channel, f.register_request("", "", "", "z9hG4bK-ask-" + std::to_string(i)));

  EXPECT_EQ(f.responses_with(f.connection, 401).size(), 2u);
  auto refused = Fixture::response_with(f.connection, 403);
  ASSERT_NE(refused, nullptr);
  EXPECT_TRUE(refused->header->contains("Retry-After"));
}

// --- Ordering and expiry (review findings 13 and 14) ---

// RFC 3261 10.3 step 7: a REGISTER from the Call-ID that last wrote a binding must carry a higher CSeq. One that
// arrives late, as a UDP retransmission of an older request or a failover's replay can, fails and undoes nothing.
TEST(RegistrarTest, AnOutOfOrderRegisterFailsAndUndoesNothing) {
  Fixture f;

  f.cseq = 9;
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_NE(f.response_with(f.connection, 200), nullptr);
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.cseq = 4;
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-late"));
  EXPECT_NE(f.response_with(f.connection, 500), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u) << "the late removal did not remove";

  // The same CSeq again is not higher either.
  f.cseq = 9;
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-same"));
  EXPECT_EQ(f.responses_with(f.connection, 500).size(), 2u);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);

  // A higher one goes through.
  f.cseq = 10;
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-next"));
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// Step 7: a binding written from another Call-ID is updated whatever the CSeq, as a client that restarted sends.
TEST(RegistrarTest, AnotherCallIdUpdatesWhateverItsCSeq) {
  Fixture f;

  f.cseq = 50;
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.cseq = 0;
  auto restarted = f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-restart");
  restarted.replace(restarted.find("Call-ID: call-registrar"), std::string("Call-ID: call-registrar").size(), "Call-ID: after-restart");
  f.receive(f.channel, restarted);
  EXPECT_EQ(f.response_with(f.connection, 500), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// Review finding 14: an expiry larger than 32 bits is the largest there is, cut to the realm's maximum, never a wrap
// to 0, which would have removed the binding it was refreshing.
TEST(RegistrarTest, AnExpiryBeyondThirtyTwoBitsIsTheRealmsMaximumNotARemoval) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "4294967296", "", "z9hG4bK-huge"));
  ASSERT_EQ(f.responses_with(f.connection, 200).size(), 2u);
  const auto locations = f.store->location_list(7);
  ASSERT_EQ(locations.size(), 1u) << "refreshed, not removed";
  EXPECT_GT(locations[0].expires_at - std::time(nullptr), 3000) << "granted the realm's 3600";

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.10:5060>;expires=4294967296", "z9hG4bK-tag"));
  EXPECT_EQ(f.store->location_list(7).size(), 1u);

  // A per-contact expires that is no number at all is refused.
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.10:5060>;expires=60x", "z9hG4bK-bad"));
  EXPECT_NE(f.response_with(f.connection, 400), nullptr);
}

// Review finding 15: the realm caps how many bindings one address of record holds. A REGISTER that would leave more
// is refused 403 and changes nothing; replacing or removing a binding is always allowed, at the cap or not.
TEST(RegistrarTest, AnAorHoldsNoMoreBindingsThanTheRealmAllows) {
  Fixture f;
  auto realm = f.store->realm_get_by_name("example.com");
  realm->registration_max_bindings = 2;
  f.store->realm_update(realm);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "<sip:alice@192.0.2.10:5060>", "z9hG4bK-1"));
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "<sip:alice@192.0.2.11:5060>", "z9hG4bK-2"));
  ASSERT_EQ(f.store->location_list(7).size(), 2u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "<sip:alice@192.0.2.12:5060>", "z9hG4bK-3"));
  EXPECT_NE(f.response_with(f.connection, 403), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 2u);

  // A refresh of one held, and a swap in one request, stay within it.
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "<sip:alice@192.0.2.10:5060>", "z9hG4bK-4"));
  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600",
                                          "<sip:alice@192.0.2.11:5060>;expires=0, <sip:alice@192.0.2.12:5060>", "z9hG4bK-5"));
  EXPECT_EQ(f.responses_with(f.connection, 200).size(), 4u);
  std::string held;
  for (const auto& location : f.store->location_list(7)) held += location.contact->to_string() + " ";
  EXPECT_EQ(f.store->location_list(7).size(), 2u) << held;
}

// RFC 3261 10.2.1: a contact's expires parameter is its own; the Expires header field is the default for the
// others. One REGISTER that removes a binding and adds another does both.
TEST(RegistrarTest, AContactsExpiresParameterIsItsOwnNotTheOthersDefault) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600", "<sip:alice@192.0.2.11:5060>", "z9hG4bK-1"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600",
                                          "<sip:alice@192.0.2.11:5060>;expires=0, <sip:alice@192.0.2.12:5060>", "z9hG4bK-2"));
  const auto held = f.store->location_list(7);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].contact->host, "192.0.2.12");
}
