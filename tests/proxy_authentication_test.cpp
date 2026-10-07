//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "headers/authorization_header.h"
#include "helpers/proxy_fixture_helper.h"
#include "servers/server.h"
#include "types/authorization.h"
#include "util.h"

using namespace athenasip;

namespace {

// A UDP listener as Core sees one, recording every flow it is asked to open.
class DatagramListener : public servers::Server {
 public:
  using Server::Server;

  void start() override {}
  void stop() override {}

  bool open_datagram_flow(boost::asio::ip::udp::endpoint remote, std::function<void(std::shared_ptr<servers::Connection>)> handler) override {
    auto connection = std::make_shared<MockConnection>("udp", remote.address().to_string(), remote.port());
    connection->reliable = false;
    connection->start();

    opened.push_back(connection);
    handler(connection);
    return true;
  }

  std::vector<std::shared_ptr<MockConnection>> opened;
};

// The policy (RFC 3261 22.3): a caller claiming an identity in a domain this node serves must prove
// it, whoever it calls; any other caller may call into this node's domains and nowhere else.
// Alice is on UDP, where a source address proves nothing, and has not registered.
struct AuthFixture : ProxyFixture {
  std::shared_ptr<DatagramListener> listener;

  std::shared_ptr<MockConnection> phone_connection;
  std::shared_ptr<athenasip::Channel> phone;

  AuthFixture() {
    listener = std::make_shared<DatagramListener>(logger, core);
    core->server_register(listener);

    phone = make_channel("198.51.100.50", &phone_connection, "udp", 5060);
    phone_connection->reliable = false;

    bind_bob();
  }

  static std::string request(const std::string& method, const std::string& to, const std::string& from, const std::string& extra = "",
                             const std::string& to_tag = "") {
    std::string raw = method + " " + to + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 198.51.100.50:5060;branch=z9hG4bK-" + method + "-" + Util::generate_random_string("", 8) + "\r\n";
    raw += "From: <" + from + ">;tag=caller\r\n";
    raw += "To: <" + to + ">" + (to_tag.empty() ? "" : ";tag=" + to_tag) + "\r\n";
    raw += "Call-ID: call-auth\r\n";
    raw += "CSeq: 1 " + method + "\r\n";
    raw += "Contact: <sip:caller@198.51.100.50:5060>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += extra;
    raw += "\r\n";
    return raw;
  }

  // RFC 2617 Digest without qop, as this node challenges: response = H(HA1:nonce:H(method:uri)).
  static std::string credentials(const std::string& user, const std::string& ha1, const std::string& nonce, const std::string& method, const std::string& uri,
                                 const std::string& realm = "example.com") {
    const auto response = Util::md5(ha1 + ":" + nonce + ":" + Util::md5(method + ":" + uri));
    return "Proxy-Authorization: Digest username=\"" + user + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" +
           response + "\", algorithm=MD5\r\n";
  }

  std::string nonce() { return mint_nonce(store->realm_get_by_name("example.com")); }

  std::shared_ptr<athenasip::SIPMessage> last_response() {
    auto all = written(phone_connection);
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
      if ((*it)->header->type == athenasip::SIPHeader::Type::Response && (*it)->header->response_code >= 200) return *it;
    }
    return nullptr;
  }

  std::size_t forwarded_off_node() const {
    std::size_t count = 0;
    for (const auto& connection : listener->opened) count += requests_with(connection, "INVITE").size();
    return count;
  }
};

}  // namespace

// RFC 3261 22.3: a caller claiming a local identity and calling off-node is answered 407 with a
// Proxy-Authenticate for the realm.
TEST(ProxyAuthenticationTest, AnOffNodeCallFromALocalIdentityIsChallenged) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:alice@example.com"));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 407);

  ASSERT_TRUE(response->header->contains("Proxy-Authenticate"));
  auto challenge = response->header->headers_map["Proxy-Authenticate"][0]->as<headers::AuthorizationHeader>();
  ASSERT_NE(challenge, nullptr);
  ASSERT_NE(challenge->value, nullptr);
  EXPECT_EQ(challenge->value->type, "Digest");
  EXPECT_EQ(challenge->value->fields["realm"], "example.com");
  EXPECT_FALSE(challenge->value->fields["nonce"].empty());

  EXPECT_EQ(f.forwarded_off_node(), 0u);
}

// A local identity calling a local user is challenged too: the From is shown to the callee.
TEST(ProxyAuthenticationTest, ALocalCallFromALocalIdentityIsChallengedToo) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com"));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 407);
  EXPECT_EQ(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// An answered challenge is forwarded with its Proxy-Authorization removed: the Digest response is
// replayable and was for this node's realm.
TEST(ProxyAuthenticationTest, AnAnsweredChallengeIsForwardedWithoutItsCredentials) {
  AuthFixture f;

  const auto nonce = f.nonce();
  ASSERT_FALSE(nonce.empty());

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com",
                                          AuthFixture::credentials("alice", "alice-ha1", nonce, "INVITE", "sip:bob@example.com")));

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_FALSE(forwarded->header->contains("Proxy-Authorization"));
}

TEST(ProxyAuthenticationTest, AnAnsweredChallengeToAnOffNodeTargetIsForwarded) {
  AuthFixture f;

  const auto nonce = f.nonce();
  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:alice@example.com",
                                          AuthFixture::credentials("alice", "alice-ha1", nonce, "INVITE", "sip:+15551234567@198.51.100.99")));

  EXPECT_EQ(f.forwarded_off_node(), 1u);
}

TEST(ProxyAuthenticationTest, AWrongAnswerIsChallengedAgain) {
  AuthFixture f;

  const auto nonce = f.nonce();
  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com",
                                          AuthFixture::credentials("alice", "not-alices-ha1", nonce, "INVITE", "sip:bob@example.com")));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 407);
  EXPECT_EQ(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// A correct response over a nonce this node did not mint is challenged again.
TEST(ProxyAuthenticationTest, ANonceThisNodeNeverMintedIsChallengedAgain) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com",
                                          AuthFixture::credentials("alice", "alice-ha1", "made-up", "INVITE", "sip:bob@example.com")));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 407);
}

// Valid credentials for one subscriber do not authorise a From naming another.
TEST(ProxyAuthenticationTest, CredentialsForSomebodyElseAreRefused) {
  AuthFixture f;

  const auto nonce = f.nonce();
  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:alice@example.com",
                                          AuthFixture::credentials("bob", "bob-ha1", nonce, "INVITE", "sip:+15551234567@198.51.100.99")));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 403);
  EXPECT_EQ(f.forwarded_off_node(), 0u);
}

// A caller from another domain calling a local user is forwarded unchallenged.
TEST(ProxyAuthenticationTest, AStrangerCallingALocalUserIsForwarded) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:mallory@elsewhere.example"));

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// A caller from another domain calling off-node is refused: this node is not an open relay.
TEST(ProxyAuthenticationTest, AStrangerCallingOffNodeIsRefused) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:mallory@elsewhere.example"));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 403);
  EXPECT_EQ(f.forwarded_off_node(), 0u);
}

// A To tag exempts a request only when it matches a dialog this node is on.
TEST(ProxyAuthenticationTest, AToTagWithNoDialogBehindItIsNoExemption) {
  AuthFixture f;

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:mallory@elsewhere.example", "", "forged"));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 403);
  EXPECT_EQ(f.forwarded_off_node(), 0u);
}

// A request inside a dialog this node is on is not challenged.
TEST(ProxyAuthenticationTest, ARequestInADialogThisNodeIsOnIsNotChallenged) {
  AuthFixture f;

  const auto nonce = f.nonce();
  f.receive(f.phone, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com",
                                          AuthFixture::credentials("alice", "alice-ha1", nonce, "INVITE", "sip:bob@example.com")));

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  std::string ok = "SIP/2.0 200 OK\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) ok += "Via: " + via->to_string() + "\r\n";
  for (const auto& route : forwarded->header->headers_map["Record-Route"]) ok += "Record-Route: " + route->to_string() + "\r\n";
  ok += "From: <sip:alice@example.com>;tag=caller\r\nTo: <sip:bob@example.com>;tag=bob\r\nCall-ID: call-auth\r\nCSeq: 1 INVITE\r\n";
  ok += "Contact: <sip:bob@192.0.2.20:5060>\r\n\r\n";
  f.receive(f.callee, ok);

  std::string routes;
  std::vector<std::string> values;
  for (const auto& route : forwarded->header->headers_map["Record-Route"]) values.push_back(route->to_string());
  for (auto it = values.rbegin(); it != values.rend(); ++it) routes += "Route: " + *it + "\r\n";

  std::string bye = "BYE sip:bob@192.0.2.20:5060 SIP/2.0\r\n";
  bye += "Via: SIP/2.0/UDP 198.51.100.50:5060;branch=z9hG4bK-bye-in-dialog\r\n";
  bye += routes;
  bye += "From: <sip:alice@example.com>;tag=caller\r\nTo: <sip:bob@example.com>;tag=bob\r\nCall-ID: call-auth\r\nCSeq: 2 BYE\r\nMax-Forwards: 70\r\n\r\n";
  f.receive(f.phone, bye);

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "BYE"), nullptr);
}

// RFC 3261 22.1: a CANCEL cannot be resubmitted with credentials, so it is never challenged and
// reaches its branch.
TEST(ProxyAuthenticationTest, ACancelIsNeverChallenged) {
  AuthFixture f;

  const auto nonce = f.nonce();
  std::string invite = "INVITE sip:bob@example.com SIP/2.0\r\n";
  invite += "Via: SIP/2.0/UDP 198.51.100.50:5060;branch=z9hG4bK-cancel-me\r\n";
  invite += "From: <sip:alice@example.com>;tag=caller\r\nTo: <sip:bob@example.com>\r\nCall-ID: call-auth\r\nCSeq: 1 INVITE\r\n";
  invite += "Contact: <sip:caller@198.51.100.50:5060>\r\nMax-Forwards: 70\r\n";
  invite += AuthFixture::credentials("alice", "alice-ha1", nonce, "INVITE", "sip:bob@example.com");
  invite += "\r\n";
  f.receive(f.phone, invite);

  ASSERT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
  f.receive(f.callee, f.response_from_callee(180, "Ringing", "bob"));

  std::string cancel = "CANCEL sip:bob@example.com SIP/2.0\r\n";
  cancel += "Via: SIP/2.0/UDP 198.51.100.50:5060;branch=z9hG4bK-cancel-me\r\n";
  cancel += "From: <sip:alice@example.com>;tag=caller\r\nTo: <sip:bob@example.com>\r\nCall-ID: call-auth\r\nCSeq: 1 CANCEL\r\nMax-Forwards: 70\r\n\r\n";
  f.receive(f.phone, cancel);

  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "CANCEL"), nullptr);
  for (const auto& message : ProxyFixture::written(f.phone_connection)) {
    EXPECT_NE(message->header->response_code, 407) << "something was challenged";
  }
}

// A connection over which a REGISTER was authenticated is trusted for that subscriber only. A UDP
// source address is not a connection and is still challenged.
TEST(ProxyAuthenticationTest, AConnectionThatRegisteredIsAuthenticatedForThatSubscriber) {
  AuthFixture f;

  std::shared_ptr<MockConnection> tcp_connection;
  auto tcp = f.make_channel("198.51.100.60", &tcp_connection, "tcp", 49152);
  f.on_strand([&]() { tcp->authenticated_as("sip:alice@example.com"); });

  f.receive(tcp, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com"));
  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

TEST(ProxyAuthenticationTest, AConnectionThatRegisteredIsAuthenticatedForThatSubscriberOnly) {
  AuthFixture f;

  std::shared_ptr<MockConnection> tcp_connection;
  auto tcp = f.make_channel("198.51.100.60", &tcp_connection, "tcp", 49152);
  f.on_strand([&]() { tcp->authenticated_as("sip:bob@example.com"); });

  f.receive(tcp, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:alice@example.com"));

  EXPECT_EQ(f.forwarded_off_node(), 0u);
  EXPECT_NE(ProxyFixture::response_with(tcp_connection, 407), nullptr);
}

TEST(ProxyAuthenticationTest, AUdpSourceThatRegisteredIsStillChallenged) {
  AuthFixture f;

  f.on_strand([&]() { f.phone->authenticated_as("sip:alice@example.com"); });

  f.receive(f.phone, AuthFixture::request("INVITE", "sip:+15551234567@198.51.100.99", "sip:alice@example.com"));

  auto response = f.last_response();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 407);
}

// The registrar marks the connection when it authenticates a REGISTER.
TEST(ProxyAuthenticationTest, TheRegistrarMarksTheConnectionItAuthenticated) {
  AuthFixture f;

  std::shared_ptr<MockConnection> tcp_connection;
  auto tcp = f.make_channel("198.51.100.60", &tcp_connection, "tcp", 49152);

  const auto nonce = f.nonce();
  const auto response = Util::md5(std::string("alice-ha1") + ":" + nonce + ":" + Util::md5("REGISTER:sip:example.com"));

  std::string reg = "REGISTER sip:example.com SIP/2.0\r\n";
  reg += "Via: SIP/2.0/TCP 198.51.100.60:49152;branch=z9hG4bK-reg\r\n";
  reg += "From: <sip:alice@example.com>;tag=reg\r\nTo: <sip:alice@example.com>\r\nCall-ID: reg-auth\r\nCSeq: 1 REGISTER\r\n";
  reg += "Contact: <sip:alice@198.51.100.60:49152;transport=tcp>\r\nExpires: 3600\r\nMax-Forwards: 70\r\n";
  reg += "Authorization: Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"sip:example.com\", response=\"" + response +
         "\", algorithm=MD5\r\n\r\n";
  f.receive(tcp, reg);

  ASSERT_NE(ProxyFixture::response_with(tcp_connection, 200), nullptr);

  f.receive(tcp, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com"));
  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// A connection presenting a certificate signed by the cluster CA is a peer node. The peer has
// already authenticated the caller, so its requests are forwarded unchallenged.
TEST(ProxyAuthenticationTest, ARequestFromAClusterPeerIsNotChallenged) {
  AuthFixture f;

  std::shared_ptr<MockConnection> peer_connection;
  auto peer = f.make_channel("198.51.100.70", &peer_connection, "tls", 5062);
  peer_connection->peer = "node-b";

  f.receive(peer, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com"));

  EXPECT_EQ(ProxyFixture::response_with(peer_connection, 407), nullptr);
  EXPECT_NE(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}

// A TLS connection that presented no cluster certificate is an endpoint and is challenged.
TEST(ProxyAuthenticationTest, ATlsConnectionThatIsNotAPeerIsChallenged) {
  AuthFixture f;

  std::shared_ptr<MockConnection> tls_connection;
  auto tls = f.make_channel("198.51.100.71", &tls_connection, "tls", 5061);

  f.receive(tls, AuthFixture::request("INVITE", "sip:bob@example.com", "sip:alice@example.com"));

  EXPECT_NE(ProxyFixture::response_with(tls_connection, 407), nullptr);
  EXPECT_EQ(ProxyFixture::request_with(f.callee_connection, "INVITE"), nullptr);
}
