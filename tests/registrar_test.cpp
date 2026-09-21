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
#include "helpers/core_fixture_helper.h"
#include "util.h"

using namespace athenasip;
using athenasip::headers::AuthorizationHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;

namespace {

// RFC 2617: HA1 is MD5(user:realm:password), and it is what the datastore stores.
const std::string kHa1 = Util::md5("alice:example.com:secret");

// RFC 8760: the same credential under SHA-256, which is a different hash of the same
// password rather than anything derivable from the first.
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
    seed_account(7, "sip:alice@example.com", kHa1);
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

  // What a client that read the SHA-256 challenge sends back.
  std::string credentials_sha256(const std::string& nonce, const std::string& uri = "sip:example.com") {
    return "Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", algorithm=SHA-256, response=\"" +
           digest_response_sha256(kHa1Sha256, nonce, "REGISTER", uri) + "\"";
  }

  std::string fresh_nonce() {
    auto realm = store->realm_get_by_name("example.com");
    return mint_nonce(realm);
  }

  // RFC 3261 10.3 step 7's registrar-configured minimum. It is off on a new realm, so a
  // test that wants one has to say so.
  void set_registration_minimum(std::uint32_t minimum) {
    auto realm = store->realm_get_by_name("example.com");
    realm->registration_minimum = minimum;
    store->realm_update(realm);
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

// An account that does not exist inside a realm we do serve is challenged rather than
// refused, so a REGISTER sweep cannot tell an absent account from a wrong password.
TEST(RegistrarTest, AnUnknownAccountInAServedRealmIsChallenged) {
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
  EXPECT_EQ(locations[0].contact->host, "192.0.2.10");
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

// RFC 8760 section 2.1: one challenge per algorithm, most preferred first. A client
// that can do better than MD5 has to be offered the chance, and one that cannot has to
// still find something it understands.
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

  // Both carry a nonce, because a client answering either has to have one.
  EXPECT_FALSE(first->value->fields["nonce"].empty());
  EXPECT_FALSE(second->value->fields["nonce"].empty());
}

// RFC 3261 25.1: algorithm is a token. Quoting it is malformed, and a client that
// checks will refuse the challenge.
TEST(RegistrarTest, TheAlgorithmIsSentAsATokenNotAQuotedString) {
  Fixture f;

  f.receive(f.channel, f.register_request());

  ASSERT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_NE(f.connection->written.find("algorithm=SHA-256"), std::string::npos);
  EXPECT_EQ(f.connection->written.find("algorithm=\"SHA-256\""), std::string::npos);
}

// The point of offering it: an account with a SHA-256 credential authenticates with one.
TEST(RegistrarTest, ASha256ResponseAuthenticates) {
  Fixture f;

  // Provisioned from a password, so it has both credentials.
  auto account = f.store->account_get(std::make_shared<types::SIPIdentity>("sip:alice@example.com"));
  ASSERT_NE(account, nullptr);
  account->ha1_sha256 = kHa1Sha256;
  ASSERT_TRUE(f.store->account_update(account));

  f.receive(f.channel, f.register_request(f.credentials_sha256(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// An account imported as a bare MD5 hash has no SHA-256 credential. Answering the
// SHA-256 challenge cannot be checked against nothing, so it is challenged again rather
// than let in or answered 500.
TEST(RegistrarTest, ASha256ResponseFromAnMd5OnlyAccountIsChallenged) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials_sha256(f.fresh_nonce())));

  EXPECT_NE(f.response_with(f.connection, 401), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// MD5 is what nearly every SIP client speaks and it keeps working, with or without the
// algorithm parameter the client may now send back.
TEST(RegistrarTest, AnMd5ResponseStillAuthenticates) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce())));

  ASSERT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// RFC 3608: the 200 OK tells the client where to send everything that follows. Without
// it a client with an unroutable Contact - a browser's always is - has nowhere to send
// its next request but the address it happened to be configured with, and a client that
// re-registers on another node keeps talking to the one it left.
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

  // The node as the flow reached it, and loose routing: a Service-Route without lr
  // would have the next hop rewrite the Request-URI on the way through.
  EXPECT_EQ(route->value->uri->host, "192.0.2.1");
  EXPECT_TRUE(route->value->uri->has_parameter("lr"));
}

// A UDP listener is bound to the wildcard, so the address the flow arrived on is
// 0.0.0.0 and a Service-Route built from it is a route the client cannot use. Found by
// the sipp harness, which received exactly that.
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

// RFC 3261 10.3 step 7: a registrar may refuse an interval shorter than it is willing to
// honour, and the refusal "MUST contain a Min-Expires header field that states the
// minimum expiration interval the registrar is willing to honor". Capping the expiry
// silently, which is what this did before, is legal but leaves a client that wanted a
// short registration with no way to learn what it may ask for.
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

  // "It then skips the remaining steps": the binding is not written.
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// The interval can also arrive on the Contact rather than in an Expires header, and step
// 7 reads that one first.
TEST(RegistrarTest, AContactExpiresParameterBelowTheMinimumIsRefusedToo) {
  Fixture f;
  f.set_registration_minimum(120);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "", "<sip:alice@192.0.2.10:5060>;expires=30"));

  EXPECT_NE(f.response_with(f.connection, 423), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// "If and only if the requested expiration interval is greater than zero AND smaller
// than one hour AND less than a registrar-configured minimum". An hour is long enough
// however the realm is configured.
TEST(RegistrarTest, AnIntervalOfAnHourIsNeverTooBrief) {
  Fixture f;
  f.set_registration_minimum(7200);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "3600"));

  EXPECT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_EQ(f.store->location_list(7).size(), 1u);
}

// Zero is a removal (10.2.1.3), not a registration that is too short to be worth
// keeping, and refusing it would leave a client unable to unregister.
TEST(RegistrarTest, AZeroExpiryIsNeverTooBrief) {
  Fixture f;
  f.set_registration_minimum(120);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "600"));
  ASSERT_EQ(f.store->location_list(7).size(), 1u);

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "0", "", "z9hG4bK-reg-2"));

  EXPECT_NE(f.response_with(f.connection, 200), nullptr);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// The RFC's own advice is that a registrar should accept brief registrations, so a realm
// that has not been given a minimum honours whatever it is asked for.
TEST(RegistrarTest, WithNoMinimumABriefRegistrationIsGranted) {
  Fixture f;

  f.receive(f.channel, f.register_request(f.credentials(f.fresh_nonce()), "30"));

  auto response = f.response_with(f.connection, 200);
  ASSERT_NE(response, nullptr);

  auto expires = response->header->headers_map["Expires"][0]->as<UIntHeader>();
  ASSERT_NE(expires, nullptr);
  EXPECT_EQ(expires->value, 30u);
}

// Step 6 skips to the last step when there is no Contact, so step 7 never runs: a query
// for the current bindings carries an Expires that is nobody's registration.
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
