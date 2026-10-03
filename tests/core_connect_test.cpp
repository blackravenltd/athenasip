//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "helpers/core_fixture_helper.h"
#include "servers/udp_server.h"

using namespace athenasip;

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;

namespace {

// Somewhere to dial. A listener that accepts and holds the connection open is all a flow
// needs on the far side; what travels over it is the transaction layer's business and is
// tested there.
struct Listener {
  net::io_context io;
  tcp::acceptor acceptor{io, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  std::thread thread;
  std::shared_ptr<tcp::socket> accepted;

  Listener() {
    _accept();
    thread = std::thread([this]() { io.run(); });
  }

  ~Listener() {
    io.stop();
    if (thread.joinable()) thread.join();
  }

  std::uint16_t port() { return acceptor.local_endpoint().port(); }

 private:
  void _accept() {
    auto socket = std::make_shared<tcp::socket>(io);
    acceptor.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
      if (!ec) accepted = socket;
      _accept();
    });
  }
};

struct ConnectFixture : CoreFixture {
  // A flow this node opens, waited on from the test thread. Production callers are the
  // handler and never wait; the contract is async because it is a name lookup and a
  // handshake.
  plugins::Result<std::shared_ptr<Channel>> connect(const std::string& transport, const std::string& host, std::uint16_t port) {
    std::promise<plugins::Result<std::shared_ptr<Channel>>> promise;
    auto future = promise.get_future();

    core->post([&]() {
      core->channel_connect(transport, host, port, [&promise](plugins::Result<std::shared_ptr<Channel>> result) { promise.set_value(std::move(result)); });
    });

    return future.get();
  }

  std::shared_ptr<Channel> find(const std::string& transport, const std::string& host, std::uint16_t port) {
    return on_strand([&]() { return core->channel_find(transport, host, port); });
  }
};

}  // namespace

// RFC 3261 16.6 step 7. Until this existed a next hop with no live flow was answered
// 480, because nothing in the tree opened a connection rather than accepting one - which
// meant a trunk or a peer node could not be reached at all.
TEST(CoreConnectTest, OpensAFlowToAHopItHasNoneTo) {
  Listener listener;
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "127.0.0.1", listener.port());

  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_NE(opened.value, nullptr);
  EXPECT_EQ(opened.value->_connection->transport_name(), "tcp");
  EXPECT_EQ(opened.value->_connection->remote_endpoint().port(), listener.port());
}

// A flow is worth having because it is reused. Dialling the same hop twice must answer
// the connection already open, or a node would hold one socket per request.
TEST(CoreConnectTest, ReusesAFlowItAlreadyHas) {
  Listener listener;
  ConnectFixture fixture;

  const auto first = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(first.ok) << first.error;

  const auto second = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(second.ok) << second.error;

  EXPECT_EQ(first.value, second.value);
}

// The flow is filed where a later request will look for it, which is what makes the
// reuse above work for a caller that never asked to open one.
TEST(CoreConnectTest, TheFlowIsFiledUnderTheHopItReached) {
  Listener listener;
  ConnectFixture fixture;

  EXPECT_EQ(fixture.find("tcp", "127.0.0.1", listener.port()), nullptr);

  const auto opened = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(opened.ok) << opened.error;

  EXPECT_EQ(fixture.find("tcp", "127.0.0.1", listener.port()), opened.value);
}

// A closed flow leaves nothing behind. The channel is filed under the address it reached
// and under the name it was dialled by, and a stale entry either way is a route to a
// socket that is gone.
TEST(CoreConnectTest, ClosingAFlowTakesEveryNameOfItAway) {
  Listener listener;
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "localhost", listener.port());
  ASSERT_TRUE(opened.ok) << opened.error;

  // Which address "localhost" resolves to is the machine's business, so the second name
  // is read from the connection rather than assumed.
  const auto reached = opened.value->_connection->remote_endpoint().address().to_string();

  // Dialled by name, reached by address: both find it.
  EXPECT_EQ(fixture.find("tcp", "localhost", listener.port()), opened.value);
  EXPECT_EQ(fixture.find("tcp", reached, listener.port()), opened.value);

  opened.value->close();
  fixture.settle();

  EXPECT_EQ(fixture.find("tcp", "localhost", listener.port()), nullptr);
  EXPECT_EQ(fixture.find("tcp", reached, listener.port()), nullptr);
}

// A hop that does not answer has to give up long before the operating system would. The
// whole transaction has thirty-two seconds (Timer B) and a fork with several bindings
// has to have room to try more than the first.
TEST(CoreConnectTest, GivesUpOnAHopThatDoesNotAnswer) {
  ConnectFixture fixture;
  fixture.config->sip_connect_timeout_ms = 250;

  const auto started = std::chrono::steady_clock::now();

  // TEST-NET-1 (RFC 5737): reserved for documentation, so nothing answers and nothing
  // refuses either - the attempt hangs until something bounds it.
  const auto opened = fixture.connect("tcp", "192.0.2.1", 5060);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(opened.ok);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

// A name that resolves to nothing is not reachable, and saying so at once is better than
// waiting out a timeout for an answer that will not come.
TEST(CoreConnectTest, ANameThatDoesNotResolveIsNotReachable) {
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "no-such-host.invalid", 5060);

  EXPECT_FALSE(opened.ok);
  EXPECT_EQ(opened.value, nullptr);
}

// A datagram to a host this node has never heard from has to leave by a listener's own
// socket, so with no UDP listener there is nothing to send it from. Saying so beats
// pretending.
TEST(CoreConnectTest, RefusesToDialATransportWithNothingToDial) {
  ConnectFixture fixture;

  const auto udp = fixture.connect("udp", "127.0.0.1", 5060);
  EXPECT_FALSE(udp.ok);

  // Outbound TLS waits for the trust configuration the cluster CA brings, and guessing at
  // it would be worse than refusing.
  const auto tls = fixture.connect("tls", "127.0.0.1", 5061);
  EXPECT_FALSE(tls.ok);
}

namespace {

// A UDP listener on an ephemeral port, and a peer on another, both on loopback.
struct DatagramFixture : ConnectFixture {
  std::shared_ptr<servers::UDPServer> listener;

  net::io_context io;
  udp::socket peer{io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0)};

  DatagramFixture() {
    listener = std::make_shared<servers::UDPServer>(logger, core, "127.0.0.1", 0);
    listener->start();
    core->server_register(listener);
  }

  ~DatagramFixture() { listener->stop(); }

  std::uint16_t peer_port() { return peer.local_endpoint().port(); }

  // One datagram, or nothing within the bound.
  std::optional<std::pair<std::string, udp::endpoint>> receive(std::chrono::milliseconds bound = std::chrono::seconds(2)) {
    const auto until = std::chrono::steady_clock::now() + bound;

    while (std::chrono::steady_clock::now() < until) {
      if (peer.available() > 0) {
        std::string data(65535, '\0');
        udp::endpoint sender;
        data.resize(peer.receive_from(net::buffer(data), sender));
        return std::make_pair(data, sender);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return std::nullopt;
  }
};

}  // namespace

// RFC 3261 18.1.1 and RFC 3581: the far end answers to the port a request came from, so a
// request this node starts over UDP leaves by the socket it listens on. A socket of its own
// would have the answer arrive somewhere nothing is reading, and a NAT in front of the far
// end would not recognise the source at all.
TEST(CoreConnectTest, AUdpFlowLeavesByTheListenersOwnSocket) {
  DatagramFixture fixture;

  const auto opened = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_NE(opened.value, nullptr);
  EXPECT_FALSE(opened.value->_connection->is_reliable());

  opened.value->write("OPTIONS sip:peer@127.0.0.1 SIP/2.0\r\n\r\n");

  const auto received = fixture.receive();
  ASSERT_TRUE(received.has_value()) << "nothing arrived";
  EXPECT_EQ(received->second.port(), fixture.listener->local_endpoint().port());
}

// And it is the same flow the answer arrives on. A listener that did not know about the flow
// it had opened would make a second one for the first datagram back, and two channels would
// be reading for one peer.
TEST(CoreConnectTest, TheAnswerToADialledUdpFlowArrivesOnIt) {
  DatagramFixture fixture;

  const auto opened = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  ASSERT_TRUE(opened.ok) << opened.error;

  // The flow's clock is the injectable one; moving it is how the stamp from the answer is
  // told apart from the one the flow was opened with.
  const auto before = opened.value->last_activity();
  fixture.timers->advance(std::chrono::seconds(1));

  const auto to = udp::endpoint(net::ip::make_address("127.0.0.1"), fixture.listener->local_endpoint().port());
  const std::string datagram = "SIP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n";
  fixture.peer.send_to(net::buffer(datagram), to);

  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < until && fixture.on_strand([&]() { return opened.value->last_activity(); }) == before) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_NE(fixture.on_strand([&]() { return opened.value->last_activity(); }), before) << "the answer did not reach the flow that was dialled";
  EXPECT_EQ(fixture.find("udp", "127.0.0.1", fixture.peer_port()), opened.value);
}

// A forgotten flow is not a dead one. Dialling the peer again once the sweep has closed
// the first flow opens a fresh one rather than handing back the closed connection.
TEST(CoreConnectTest, AUdpFlowCanBeDialledAgainAfterItIsForgotten) {
  DatagramFixture fixture;

  const auto first = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  ASSERT_TRUE(first.ok) << first.error;

  fixture.on_strand([&]() { first.value->close(); });
  fixture.settle();

  const auto second = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  ASSERT_TRUE(second.ok) << second.error;
  EXPECT_NE(second.value, first.value);
  EXPECT_TRUE(second.value->_connection->is_open());

  second.value->write("OPTIONS sip:peer@127.0.0.1 SIP/2.0\r\n\r\n");
  EXPECT_TRUE(fixture.receive().has_value());
}
