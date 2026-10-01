//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <array>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "headers/via_header.h"
#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

// Somewhere for the node to fall back to. It accepts and reads, which is all RFC 3261
// 18.1.1 needs of the far end: what matters is whether this node moved the request off
// UDP, not what the far end did with it afterwards.
//
// Everything on the socket happens on the listener's own thread and what arrived is
// handed to the test under a lock. Reaching into the socket from the test thread is a
// data race, whatever it looks like it is reading.
struct TcpListener {
  net::io_context io;
  tcp::acceptor acceptor{io, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  std::thread thread;

  TcpListener() {
    _accept();
    thread = std::thread([this]() { io.run(); });
  }

  ~TcpListener() {
    io.stop();
    if (thread.joinable()) thread.join();
  }

  std::uint16_t port() { return acceptor.local_endpoint().port(); }

  std::string received() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _received;
  }

 private:
  void _accept() {
    auto socket = std::make_shared<tcp::socket>(io);
    acceptor.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
      if (!ec) _read(socket);
      _accept();
    });
  }

  void _read(const std::shared_ptr<tcp::socket>& socket) {
    auto buffer = std::make_shared<std::array<char, 4096>>();

    socket->async_read_some(net::buffer(*buffer), [this, socket, buffer](const boost::system::error_code& ec, std::size_t length) {
      if (ec) return;

      {
        std::lock_guard<std::mutex> lock(_mutex);
        _received.append(buffer->data(), length);
      }

      _read(socket);
    });
  }

  std::mutex _mutex;
  std::string _received;
};

// Alice calls Bob, both on UDP, with Bob's flow pointed at a loopback port a test can
// put a real TCP listener on. Everything about 18.1.1 is a decision this node makes on
// the way out, so the fixture only has to make the two transports real enough to tell
// apart.
struct DatagramFixture : CoreFixture {
  std::shared_ptr<MockConnection> caller_connection;
  std::shared_ptr<athenasip::Channel> caller;

  std::shared_ptr<MockConnection> callee_connection;
  std::shared_ptr<athenasip::Channel> callee;

  std::shared_ptr<athenasip::types::Account> bob;

  explicit DatagramFixture(std::uint16_t callee_port) {
    seed_realm("example.com");
    seed_account(1, "sip:alice@example.com", "alice-ha1");
    bob = seed_account(2, "sip:bob@example.com", "bob-ha1");

    caller = make_channel("127.0.0.1", &caller_connection, "udp", 5070);
    callee = make_channel("127.0.0.1", &callee_connection, "udp", callee_port);

    register_binding(bob, std::make_shared<athenasip::types::SIPUri>("sip:bob@127.0.0.1:" + std::to_string(callee_port)), callee, 3600);

    // Alice registered over her connection, so her calls are not challenged; what is under
    // test here is where the INVITE leaves by, not whether she may send it.
    on_strand([this]() { caller->authenticated_as("sip:alice@example.com"); });
  }

  // An INVITE whose session description is long enough to push the forwarded request past
  // 1300 bytes. A description this size is ordinary: a browser's offer with ICE candidates
  // and a DTLS fingerprint is larger still, which is why the rule exists.
  std::string large_invite() {
    std::string sdp =
        "v=0\r\n"
        "o=alice 2890844526 2890844526 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=audio 49170 RTP/AVP 0 8 9 18 96 97 98 99 100 101 102 103 104 105 106 107\r\n";

    for (int payload = 96; payload < 116; ++payload) {
      sdp += "a=rtpmap:" + std::to_string(payload) + " codec" + std::to_string(payload) + "/48000/2\r\n";
      sdp += "a=fmtp:" + std::to_string(payload) + " useinbandfec=1;minptime=10;maxplaybackrate=48000;stereo=1;cbr=0\r\n";
    }

    return _invite_with(sdp);
  }

  std::string small_invite() {
    return _invite_with(
        "v=0\r\n"
        "o=alice 2890844526 2890844526 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=audio 49170 RTP/AVP 0\r\n");
  }

  // The flow this node opened to the callee's address over TCP, if it opened one.
  std::shared_ptr<athenasip::Channel> tcp_flow_to(std::uint16_t port) {
    return on_strand([&]() { return core->channel_find("tcp", "127.0.0.1", port); });
  }

 private:
  std::string _invite_with(const std::string& sdp) {
    std::string raw = "INVITE sip:bob@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 127.0.0.1:5070;branch=z9hG4bK-datagram\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>\r\n";
    raw += "Call-ID: call-datagram\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "Contact: <sip:alice@127.0.0.1:5070>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "Content-Type: application/sdp\r\n";
    raw += "Content-Length: " + std::to_string(sdp.size()) + "\r\n";
    raw += "\r\n";
    raw += sdp;
    return raw;
  }
};

std::string top_via_of(const std::shared_ptr<athenasip::SIPMessage>& message) {
  if (!message || !message->header->contains("Via")) return "";

  auto via = message->header->headers_map["Via"][0]->as<athenasip::headers::ViaHeader>();
  return via == nullptr ? "" : via->version;
}

}  // namespace

// RFC 3261 18.1.1: a request larger than 1300 bytes, with the path MTU unknown, MUST be
// sent over a congestion controlled transport. Sending it as a datagram invites IP
// fragmentation, and a fragmented SIP request is one lost fragment away from a call that
// silently never happens.
TEST(ProxyDatagramTest, AnOversizedRequestLeavesOverTcpInsteadOfUdp) {
  TcpListener listener;
  DatagramFixture fixture(listener.port());

  fixture.receive(fixture.caller, fixture.large_invite());
  fixture.settle();

  // Nothing went out as a datagram.
  EXPECT_EQ(ProxyFixture::request_with(fixture.callee_connection, "INVITE"), nullptr);

  // And the node opened a TCP flow to the same hop to carry it.
  EXPECT_NE(fixture.tcp_flow_to(listener.port()), nullptr);
}

// "If this causes a change in the transport protocol from the one indicated in the top
// Via, the value in the top Via MUST be changed." A Via that names UDP when the request
// left over TCP sends the answer to a port nothing is listening on.
TEST(ProxyDatagramTest, TheTopViaSaysWhereTheRequestReallyWent) {
  TcpListener listener;
  DatagramFixture fixture(listener.port());

  fixture.receive(fixture.caller, fixture.large_invite());
  fixture.settle();

  auto flow = fixture.tcp_flow_to(listener.port());
  ASSERT_NE(flow, nullptr);

  // Read what this node actually wrote to the far end off the wire.
  std::string raw;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  while (raw.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    raw = listener.received();
    if (raw.find("\r\n\r\n") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ASSERT_NE(raw.find("INVITE"), std::string::npos) << "nothing arrived over the TCP flow";

  const auto split = raw.find("\r\n\r\n");
  auto forwarded = std::make_shared<athenasip::SIPMessage>();
  forwarded->header = std::make_shared<athenasip::SIPHeader>(raw.substr(0, split));

  EXPECT_EQ(top_via_of(forwarded), "SIP/2.0/TCP");
}

// The rule is about size and nothing else. A request that fits goes out the way it was
// routed, and a node that opened a TCP connection for every call would be paying a
// handshake for nothing.
TEST(ProxyDatagramTest, ARequestThatFitsStaysOnUdp) {
  TcpListener listener;
  DatagramFixture fixture(listener.port());

  fixture.receive(fixture.caller, fixture.small_invite());
  fixture.settle();

  auto forwarded = ProxyFixture::request_with(fixture.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr);

  EXPECT_EQ(top_via_of(forwarded), "SIP/2.0/UDP");
  EXPECT_EQ(fixture.tcp_flow_to(listener.port()), nullptr);
}

// 18.1.1 again: "if the attempt to establish the connection ... results in a TCP reset,
// the element SHOULD retry the request, using UDP". A datagram that may be fragmented
// beats a request that never leaves.
TEST(ProxyDatagramTest, ARequestFallsBackToUdpWhenTcpIsRefused) {
  // A port nothing is listening on, so the connection is refused at once.
  const std::uint16_t closed_port = 9;

  DatagramFixture fixture(closed_port);
  fixture.config->sip_connect_timeout_ms = 500;

  fixture.receive(fixture.caller, fixture.large_invite());
  fixture.settle();

  auto forwarded = ProxyFixture::request_with(fixture.callee_connection, "INVITE");
  ASSERT_NE(forwarded, nullptr) << "the request never left";

  EXPECT_EQ(top_via_of(forwarded), "SIP/2.0/UDP");
}
