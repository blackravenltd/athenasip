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

#include "events/topics.h"
#include "helpers/core_fixture_helper.h"
#include "util.h"

using namespace athenasip;

namespace {

const std::string kHa1 = Util::md5("alice:example.com:secret");

// AthenaSIP-Alternate-Server: the other nodes a client can register with, in the 2xx to its REGISTER, under the
// conditions the plan sets for it.
struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> connection;
  std::shared_ptr<Channel> channel;

  explicit Fixture(const std::string& transport = "tls") {
    config->events_status_interval = 30;
    seed_realm("example.com");
    seed_subscriber(7, "sip:alice@example.com", kHa1);
    channel = make_channel("192.0.2.10", &connection, transport, 50000);
  }

  void hear(const std::string& node, const std::string& state = "ok") {
    const auto report = R"({"status":")" + state + R"(","node":")" + node + R"(","version":"1.0.0","at":"2026-10-04T10:00:00Z","transports":[)" +
                        R"({"transport":"udp","address":"203.0.113.9","port":5060,"uri":"sip:203.0.113.9:5060"},)" +
                        R"({"transport":"tls","address":"203.0.113.9","port":5061,"uri":"sips:203.0.113.9:5061;transport=tls"},)" +
                        R"({"transport":"wss","address":"203.0.113.9","port":9443,"uri":"sips:203.0.113.9:9443;transport=wss"}]})";
    on_strand([this, node, report]() { core->nodes()->observe(events::topics::node_status(node), report); });
  }

  std::shared_ptr<SIPMessage> register_with(const std::string& supported = "athenasip-failover", const std::string& expires = "600") {
    connection->written.clear();

    const auto nonce = mint_nonce(store->realm_get_by_name("example.com"));
    const std::string uri = "sip:example.com";
    const auto response = Util::md5(kHa1 + ":" + nonce + ":" + Util::md5("REGISTER:" + uri));

    std::string raw = "REGISTER " + uri + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TLS 192.0.2.10:50000;branch=z9hG4bK-reg-" + nonce.substr(0, 8) + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-registrar-failover\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:50000;transport=tls>\r\n";
    raw += "Expires: " + expires + "\r\n";
    if (!supported.empty()) raw += "Supported: " + supported + "\r\n";
    raw += "Authorization: Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" + response + "\"\r\n";
    raw += "\r\n";

    receive(channel, raw);
    return response_with(connection, 200);
  }

  static std::vector<std::string> alternates(const std::shared_ptr<SIPMessage>& response) {
    std::vector<std::string> values;
    if (!response || !response->header->contains("AthenaSIP-Alternate-Server")) return values;
    for (const auto& value : response->header->headers_map["AthenaSIP-Alternate-Server"]) values.push_back(value->to_string());
    return values;
  }
};

}  // namespace

// A client that asked for it, over TLS, is told the other nodes' TLS addresses, each with how long the list is
// good for: the lifetime of the registration it came with.
TEST(RegistrarFailoverTest, AClientThatAskedOverTlsIsToldTheOtherNodes) {
  Fixture f;
  f.hear("node-b");

  EXPECT_EQ(Fixture::alternates(f.register_with()), std::vector<std::string>{"<sips:203.0.113.9:5061;transport=tls>;expires=600"});
}

// Over WSS, the other nodes' secure WebSockets.
TEST(RegistrarFailoverTest, OverWssTheAlternatesAreSecureWebSockets) {
  Fixture f("wss");
  f.hear("node-b");

  EXPECT_EQ(Fixture::alternates(f.register_with()), std::vector<std::string>{"<sips:203.0.113.9:9443;transport=wss>;expires=600"});
}

// Over a transport that did not authenticate the server, the header would be a redirection anyone could forge.
TEST(RegistrarFailoverTest, NothingIsSaidOverATransportThatDidNotAuthenticateTheServer) {
  Fixture f("tcp");
  f.hear("node-b");

  EXPECT_TRUE(Fixture::alternates(f.register_with()).empty());
}

// Only a client that advertised the option tag gets the header.
TEST(RegistrarFailoverTest, NothingIsSaidToAClientThatDidNotAsk) {
  Fixture f;
  f.hear("node-b");

  EXPECT_TRUE(Fixture::alternates(f.register_with("outbound")).empty());
}

// A node that is not up is not somewhere to go.
TEST(RegistrarFailoverTest, ANodeThatIsNotUpIsNotListed) {
  Fixture f;
  f.hear("node-b", "degraded");

  EXPECT_TRUE(Fixture::alternates(f.register_with()).empty());
}

// This node is not its own alternate.
TEST(RegistrarFailoverTest, ThisNodeIsNotListed) {
  Fixture f;
  f.hear("test-node");

  EXPECT_TRUE(Fixture::alternates(f.register_with()).empty());
}
