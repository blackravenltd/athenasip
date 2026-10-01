//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dns/sip_locator.h"
#include "headers/sip_identity_header.h"
#include "helpers/fake_resolver_helper.h"
#include "helpers/proxy_fixture_helper.h"
#include "servers/server.h"
#include "types/sip_uri.h"

using namespace athenasip;

namespace {

// A UDP listener as far as Core can tell: it can open a flow to a peer it has not heard
// from, through what would be its own socket. Each one it opens is kept so a test can see
// where it was asked to send and what went.
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

  std::shared_ptr<MockConnection> opened_to(const std::string& address, std::uint16_t port) const {
    for (const auto& connection : opened) {
      if (connection->remote_endpoint().address().to_string() == address && connection->remote_endpoint().port() == port) return connection;
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<MockConnection>> opened;
};

// Bob is a phone behind a NAT, registered over UDP. The Contact he wrote names his own side
// of the NAT, which nothing outside it can reach; the REGISTER arrived from the NAT's
// mapping of it, which is the only address that reaches him (RFC 3581, RFC 5626 section 3).
struct NatFixture : ProxyFixture {
  static constexpr const char* kPrivateContact = "sip:bob@192.168.1.5:5060";

  std::shared_ptr<DatagramListener> listener;

  std::shared_ptr<MockConnection> phone_connection;
  std::shared_ptr<athenasip::Channel> phone;

  NatFixture() {
    listener = std::make_shared<DatagramListener>(logger, core);
    core->server_register(listener);

    phone = make_channel("203.0.113.7", &phone_connection, "udp", 40000);
    phone_connection->reliable = false;

    register_binding(bob, std::make_shared<types::SIPUri>(kPrivateContact), phone, 3600);
  }

  // What the idle sweep does to a UDP flow that has been quiet for five minutes: this node
  // forgets it. The NAT has not, and nor has the phone.
  void forget_the_phones_flow() {
    on_strand([this]() { phone->close(); });
    settle();
  }
};

}  // namespace

// The control. While the node still holds the flow, the INVITE goes down it and nothing new
// is opened.
TEST(ProxyNatTest, ALiveUdpFlowIsUsedAsItIs) {
  NatFixture f;

  f.receive(f.caller, f.invite());

  EXPECT_NE(ProxyFixture::request_with(f.phone_connection, "INVITE"), nullptr);
  EXPECT_TRUE(f.listener->opened.empty());
}

// RFC 5626 section 5.3: a request for a binding goes over the flow the binding was
// registered on, and for UDP that flow is the pair of addresses (section 3.1) - it does not
// end because this node stopped keeping a record of it. The Request-URI stays the Contact;
// only the hop is the flow's.
//
// Before this, a forgotten UDP flow fell back to the Contact, which for a phone behind a
// NAT is an address on somebody else's LAN, and the call failed 480 five minutes after the
// phone last said anything.
TEST(ProxyNatTest, AUdpBindingIsReachedWhereItRegisteredFromOnceItsFlowIsForgotten) {
  NatFixture f;
  f.forget_the_phones_flow();

  f.receive(f.caller, f.invite());

  auto reopened = f.listener->opened_to("203.0.113.7", 40000);
  ASSERT_NE(reopened, nullptr) << "the INVITE did not go back to the address the phone registered from";

  auto forwarded = ProxyFixture::request_with(reopened, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "192.168.1.5");

  EXPECT_EQ(f.listener->opened_to("192.168.1.5", 5060), nullptr) << "the private Contact was tried as well";
}

// The flow is the holding node's. Another node sending from its own socket comes from an
// address the phone's NAT has never seen, and a NAT that filters by source drops it.
TEST(ProxyNatTest, AnotherNodesForgottenFlowIsNotReopenedFromHere) {
  NatFixture f;
  f.forget_the_phones_flow();

  // The binding was written by a node of another name.
  f.config->sip_node_id = "another-node";

  f.receive(f.caller, f.invite());

  EXPECT_EQ(f.listener->opened_to("203.0.113.7", 40000), nullptr);
}

// The same for a request inside a dialog. Alice called from behind a NAT over UDP, the
// call has gone on longer than the idle sweep, and Bob hangs up. The Record-Route this node
// wrote carries a token for Alice's flow, and it has to still say where that flow went;
// her Contact is on her LAN.
TEST(ProxyNatTest, AByeReachesAUdpCallerWhoseFlowWasForgottenDuringTheCall) {
  ProxyFixture f;

  auto listener = std::make_shared<DatagramListener>(f.logger, f.core);
  f.core->server_register(listener);

  std::shared_ptr<MockConnection> alice_connection;
  auto alice = f.make_channel("203.0.113.9", &alice_connection, "udp", 41000);
  alice_connection->reliable = false;

  f.bind_bob();

  std::string invite = "INVITE sip:bob@example.com SIP/2.0\r\n";
  invite += "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-nat-invite;rport\r\n";
  invite += "From: <sip:alice@example.com>;tag=alice\r\n";
  invite += "To: <sip:bob@example.com>\r\n";
  invite += "Call-ID: call-proxy\r\n";
  invite += "CSeq: 1 INVITE\r\n";
  invite += "Contact: <sip:alice@10.0.0.9:5060>\r\n";
  invite += "Max-Forwards: 70\r\n";
  invite += "\r\n";

  // A phone on UDP proves who it is by answering the challenge; a source address cannot.
  f.receive(alice, f.with_credentials(invite, "alice", "alice-ha1"));
  f.receive(f.callee, f.response_from_callee(200, "OK"));
  f.settle();

  auto forwarded = ProxyFixture::request_with(f.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  // Bob's route set: the Record-Route values in the order the request carried them
  // (RFC 3261 12.1.1).
  std::string bye = "BYE sip:alice@10.0.0.9:5060 SIP/2.0\r\n";
  bye += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=z9hG4bK-nat-bye\r\n";
  for (const auto& value : forwarded->header->headers_map["Record-Route"]) bye += "Route: " + value->to_string() + "\r\n";
  bye += "From: <sip:bob@example.com>;tag=bob\r\n";
  bye += "To: <sip:alice@example.com>;tag=alice\r\n";
  bye += "Call-ID: call-proxy\r\n";
  bye += "CSeq: 2 BYE\r\n";
  bye += "Max-Forwards: 70\r\n";
  bye += "\r\n";

  // Five quiet minutes into the call.
  f.on_strand([&alice]() { alice->close(); });
  f.settle();

  f.receive(f.callee, bye);

  auto reopened = listener->opened_to("203.0.113.9", 41000);
  ASSERT_NE(reopened, nullptr) << "the BYE did not go back to where Alice's INVITE came from";

  auto delivered = ProxyFixture::request_with(reopened, "BYE");
  ASSERT_NE(delivered, nullptr);
  EXPECT_EQ(delivered->header->request_uri->host, "10.0.0.9");

  EXPECT_EQ(listener->opened_to("10.0.0.9", 5060), nullptr) << "the private Contact was tried as well";
}

// RFC 3261 16.7 step 1 sends a response with no transaction here back down the Via chain
// statelessly, and 18.2.2 with RFC 3581 section 4 says where: over UDP, to received and
// rport. That is an address and a port, not a record this node has to still be keeping -
// the flow it names may have been forgotten, and the NAT in front of it has not.
TEST(ProxyNatTest, AStrayResponseGoesToReceivedAndRportEvenWhenTheFlowIsForgotten) {
  ProxyFixture f;

  auto listener = std::make_shared<DatagramListener>(f.logger, f.core);
  f.core->server_register(listener);

  std::string stray = "SIP/2.0 200 OK\r\n";
  stray += "Via: SIP/2.0/UDP 192.0.2.1:5060;branch=z9hG4bK-gone\r\n";
  stray += "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-caller;received=203.0.113.9;rport=41000\r\n";
  stray += "From: <sip:alice@example.com>;tag=alice\r\n";
  stray += "To: <sip:bob@example.com>;tag=bob\r\n";
  stray += "Call-ID: call-stray\r\n";
  stray += "CSeq: 1 INVITE\r\n";
  stray += "\r\n";

  f.receive(f.callee, stray);

  auto reopened = listener->opened_to("203.0.113.9", 41000);
  ASSERT_NE(reopened, nullptr) << "the response did not go to received and rport";
  EXPECT_NE(ProxyFixture::response_with(reopened, 200), nullptr);
  EXPECT_EQ(listener->opened_to("10.0.0.9", 5060), nullptr) << "the sent-by was tried as well";
}

// The Via chain is anybody's to write. An rport that is not a port is dropped with the
// response it came in, rather than thrown out of the parser and taking the node with it.
TEST(ProxyNatTest, AStrayResponseWithAnRportThatIsNotAPortIsDropped) {
  ProxyFixture f;

  auto listener = std::make_shared<DatagramListener>(f.logger, f.core);
  f.core->server_register(listener);

  for (const std::string rport : {"abc", "99999999999999999999", "70000", "0"}) {
    std::string stray = "SIP/2.0 200 OK\r\n";
    stray += "Via: SIP/2.0/UDP 192.0.2.1:5060;branch=z9hG4bK-gone\r\n";
    stray += "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-caller;received=203.0.113.9;rport=" + rport + "\r\n";
    stray += "From: <sip:alice@example.com>;tag=alice\r\n";
    stray += "To: <sip:bob@example.com>;tag=bob\r\n";
    stray += "Call-ID: call-stray\r\n";
    stray += "CSeq: 1 INVITE\r\n";
    stray += "\r\n";

    f.receive(f.callee, stray);
  }

  EXPECT_TRUE(listener->opened.empty());
}

// RFC 3263, through the proxy: a request for a URI naming a host goes where DNS says, in
// the order it says, and a place that cannot be reached is passed over for the next (4.3).
// NAPTR puts TCP first here and the TCP server is not listening, so the INVITE leaves by
// UDP to the second service's host.
TEST(ProxyNatTest, AHostIsLocatedByDnsAndAnUnreachableHopIsPassedOver) {
  ProxyFixture f;

  auto listener = std::make_shared<DatagramListener>(f.logger, f.core);
  f.core->server_register(listener);

  auto resolver = std::make_shared<FakeResolver>();
  resolver->naptr("trunk.example.net", 10, 10, "SIP+D2T", "_sip._tcp.trunk.example.net");
  resolver->naptr("trunk.example.net", 20, 10, "SIP+D2U", "_sip._udp.trunk.example.net");
  resolver->srv("_sip._tcp.trunk.example.net", 0, 0, 1, "closed.trunk.example.net");
  resolver->srv("_sip._udp.trunk.example.net", 0, 0, 5080, "media.trunk.example.net");
  resolver->a("closed.trunk.example.net", "127.0.0.1");
  resolver->a("media.trunk.example.net", "198.51.100.80");
  f.core->locator_set(std::make_shared<dns::SipLocator>(resolver));

  f.receive(f.caller, f.invite("z9hG4bK-trunk", "sip:+15551234567@trunk.example.net"));

  // The TCP attempt is a real connect to a port nothing listens on, so it is refused by
  // the kernel rather than by the clock; give it a moment either way.
  for (int i = 0; i < 200 && !listener->opened_to("198.51.100.80", 5080); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    f.settle();
  }

  auto reached = listener->opened_to("198.51.100.80", 5080);
  ASSERT_NE(reached, nullptr) << "the request did not reach the host DNS named";

  auto forwarded = ProxyFixture::request_with(reached, "INVITE");
  ASSERT_NE(forwarded, nullptr);
  EXPECT_EQ(forwarded->header->request_uri->host, "trunk.example.net") << "the Request-URI is the URI, not the hop";
  EXPECT_TRUE(resolver->was_asked("trunk.example.net", dns::Type::NAPTR));
}

namespace {

// A trunk known by name, served by two hosts of equal standing (RFC 2782 priorities 10 and
// 20), both on UDP.
struct TwoHopFixture : ProxyFixture {
  std::shared_ptr<DatagramListener> listener;

  TwoHopFixture() {
    listener = std::make_shared<DatagramListener>(logger, core);
    core->server_register(listener);

    auto resolver = std::make_shared<FakeResolver>();
    resolver->srv("_sip._udp.trunk.example.net", 10, 0, 5060, "one.trunk.example.net");
    resolver->srv("_sip._udp.trunk.example.net", 20, 0, 5060, "two.trunk.example.net");
    resolver->a("one.trunk.example.net", "198.51.100.1");
    resolver->a("two.trunk.example.net", "198.51.100.2");
    core->locator_set(std::make_shared<dns::SipLocator>(resolver));
  }

  void call() { receive(caller, invite("z9hG4bK-two-hops", "sip:+15551234567@trunk.example.net")); }

  std::shared_ptr<athenasip::Channel> hop_channel(const std::string& address) {
    return on_strand([this, address]() { return core->channel_find("udp", address, 5060); });
  }

  void answer_from(const std::string& address, int code, const std::string& reason) {
    auto connection = listener->opened_to(address, 5060);
    ASSERT_NE(connection, nullptr);
    auto forwarded = ProxyFixture::request_with(connection, "INVITE");
    ASSERT_NE(forwarded, nullptr);

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\nTo: <sip:+15551234567@trunk.example.net>;tag=trunk\r\n";
    raw += "Call-ID: call-proxy\r\nCSeq: 1 INVITE\r\n\r\n";

    receive(hop_channel(address), raw);
  }
};

}  // namespace

// RFC 3263 4.3: "If a 503 ... is received, the element SHOULD try the next element in the
// list". The server was too busy, not the call refused, and the next server may not be.
TEST(ProxyNatTest, A503FromOneHopTriesTheNextHopOfTheSameTarget) {
  TwoHopFixture f;
  f.call();

  ASSERT_NE(f.listener->opened_to("198.51.100.1", 5060), nullptr);
  ASSERT_EQ(f.listener->opened_to("198.51.100.2", 5060), nullptr) << "the second hop is for when the first fails";

  f.answer_from("198.51.100.1", 503, "Service Unavailable");

  auto second = f.listener->opened_to("198.51.100.2", 5060);
  ASSERT_NE(second, nullptr);
  auto retried = ProxyFixture::request_with(second, "INVITE");
  ASSERT_NE(retried, nullptr);
  EXPECT_EQ(retried->header->request_uri->host, "trunk.example.net");

  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 503), nullptr);
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 500), nullptr) << "the caller was answered before the next hop had its turn";
}

// 4.3: "... or a transaction timeout ..." - a server that never answered.
TEST(ProxyNatTest, ATimeoutOnOneHopTriesTheNextHopOfTheSameTarget) {
  TwoHopFixture f;
  f.call();

  ASSERT_NE(f.listener->opened_to("198.51.100.1", 5060), nullptr);

  // Timer B, 64*T1, on the injectable clock.
  f.timers->advance(std::chrono::seconds(33));
  f.settle();

  auto second = f.listener->opened_to("198.51.100.2", 5060);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(ProxyFixture::request_with(second, "INVITE"), nullptr);
}

// But a 486 is the far end saying no, which another server for the same domain would say
// too. Failover is for a server failing, not for an answer.
TEST(ProxyNatTest, AnAnswerFromAHopIsNotFailedOver) {
  TwoHopFixture f;
  f.call();

  f.answer_from("198.51.100.1", 486, "Busy Here");

  EXPECT_EQ(f.listener->opened_to("198.51.100.2", 5060), nullptr);
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 486), nullptr);
}

// When every hop has failed, the caller is told, and a 503 is not passed upstream as one
// (RFC 3261 16.7 step 6: it would make the caller think this node was overloaded).
TEST(ProxyNatTest, WhenEveryHopFailsTheCallerIsAnswered) {
  TwoHopFixture f;
  f.call();

  f.answer_from("198.51.100.1", 503, "Service Unavailable");
  f.answer_from("198.51.100.2", 503, "Service Unavailable");

  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 503), nullptr);
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 500), nullptr);
}

// A desk phone with a routable address, registered over UDP, whose flow this node has
// forgotten: the flow's address and the Contact are the same place, and it is reached.
// Before outbound UDP existed this failed too, because nothing could open a UDP flow at all.
TEST(ProxyNatTest, AForgottenUdpFlowWithARoutableContactIsReached) {
  ProxyFixture f;

  auto listener = std::make_shared<DatagramListener>(f.logger, f.core);
  f.core->server_register(listener);

  std::shared_ptr<MockConnection> desk_connection;
  auto desk = f.make_channel("198.51.100.30", &desk_connection, "udp", 5060);
  desk_connection->reliable = false;

  f.register_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@198.51.100.30:5060"), desk, 3600);

  f.on_strand([&desk]() { desk->close(); });
  f.settle();

  f.receive(f.caller, f.invite());

  auto reopened = listener->opened_to("198.51.100.30", 5060);
  ASSERT_NE(reopened, nullptr);
  EXPECT_NE(ProxyFixture::request_with(reopened, "INVITE"), nullptr);
}
