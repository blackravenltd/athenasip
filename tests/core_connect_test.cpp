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

// A loopback TCP listener that accepts connections and holds them open.
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
  // Blocks the test thread on the asynchronous channel_connect.
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

// RFC 3261 16.6 step 7: a next hop with no live flow is dialled.
TEST(CoreConnectTest, OpensAFlowToAHopItHasNoneTo) {
  Listener listener;
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "127.0.0.1", listener.port());

  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_NE(opened.value, nullptr);
  EXPECT_EQ(opened.value->_connection->transport_name(), "tcp");
  EXPECT_EQ(opened.value->_connection->remote_endpoint().port(), listener.port());
}

// Dialling the same hop twice returns the flow already open.
TEST(CoreConnectTest, ReusesAFlowItAlreadyHas) {
  Listener listener;
  ConnectFixture fixture;

  const auto first = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(first.ok) << first.error;

  const auto second = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(second.ok) << second.error;

  EXPECT_EQ(first.value, second.value);
}

// A dialled flow is registered where channel_find looks for it.
TEST(CoreConnectTest, TheFlowIsFiledUnderTheHopItReached) {
  Listener listener;
  ConnectFixture fixture;

  EXPECT_EQ(fixture.find("tcp", "127.0.0.1", listener.port()), nullptr);

  const auto opened = fixture.connect("tcp", "127.0.0.1", listener.port());
  ASSERT_TRUE(opened.ok) << opened.error;

  EXPECT_EQ(fixture.find("tcp", "127.0.0.1", listener.port()), opened.value);
}

// A flow is registered under both the name it was dialled by and the address it reached; closing
// it removes both.
TEST(CoreConnectTest, ClosingAFlowTakesEveryNameOfItAway) {
  Listener listener;
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "localhost", listener.port());
  ASSERT_TRUE(opened.ok) << opened.error;

  // What "localhost" resolves to is machine-dependent, so read the address from the connection.
  const auto reached = opened.value->_connection->remote_endpoint().address().to_string();

  EXPECT_EQ(fixture.find("tcp", "localhost", listener.port()), opened.value);
  EXPECT_EQ(fixture.find("tcp", reached, listener.port()), opened.value);

  opened.value->close();
  fixture.settle();

  EXPECT_EQ(fixture.find("tcp", "localhost", listener.port()), nullptr);
  EXPECT_EQ(fixture.find("tcp", reached, listener.port()), nullptr);
}

// A connect is bounded by sip_connect_timeout_ms, well inside Timer B, so a fork can try other bindings.
TEST(CoreConnectTest, GivesUpOnAHopThatDoesNotAnswer) {
  ConnectFixture fixture;
  fixture.config->sip_connect_timeout_ms = 250;

  const auto started = std::chrono::steady_clock::now();

  // TEST-NET-1 (RFC 5737): nothing answers or refuses, so only the timeout ends the attempt.
  const auto opened = fixture.connect("tcp", "192.0.2.1", 5060);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(opened.ok);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

// A name that does not resolve fails the connect.
TEST(CoreConnectTest, ANameThatDoesNotResolveIsNotReachable) {
  ConnectFixture fixture;

  const auto opened = fixture.connect("tcp", "no-such-host.invalid", 5060);

  EXPECT_FALSE(opened.ok);
  EXPECT_EQ(opened.value, nullptr);
}

// UDP is sent from a listener's socket, so with no UDP listener the connect fails.
TEST(CoreConnectTest, RefusesToDialATransportWithNothingToDial) {
  ConnectFixture fixture;

  const auto udp = fixture.connect("udp", "127.0.0.1", 5060);
  EXPECT_FALSE(udp.ok);

  // TLS is dialled only with the cluster's trust configuration, and this node has none.
  const auto tls = fixture.connect("tls", "127.0.0.1", 5061);
  EXPECT_FALSE(tls.ok);
}

// sip.allow_unencrypted: false: the node dials no plain transport.
TEST(CoreConnectTest, RefusingUnencryptedSipDialsNoPlainTransport) {
  Listener listener;
  ConnectFixture fixture;
  fixture.config->sip_allow_unencrypted = false;

  const auto tcp = fixture.connect("tcp", "127.0.0.1", listener.port());
  EXPECT_FALSE(tcp.ok);
  EXPECT_EQ(fixture.find("tcp", "127.0.0.1", listener.port()), nullptr);
}

namespace {

// A UDP listener and a peer socket, both on loopback ephemeral ports.
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

// RFC 3261 18.1.1, RFC 3581: a UDP request leaves from the listening socket, since the far end
// answers to its source port.
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

// sip.allow_unencrypted: false: no UDP flow is dialled, even with a UDP socket to send from.
TEST(CoreConnectTest, RefusingUnencryptedSipDialsNoUdpFlow) {
  DatagramFixture fixture;
  fixture.config->sip_allow_unencrypted = false;

  const auto opened = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  EXPECT_FALSE(opened.ok);
  EXPECT_FALSE(fixture.receive(std::chrono::milliseconds(200)).has_value());
}

// The answer arrives on the dialled flow; the listener does not make a second channel for the peer.
TEST(CoreConnectTest, TheAnswerToADialledUdpFlowArrivesOnIt) {
  DatagramFixture fixture;

  const auto opened = fixture.connect("udp", "127.0.0.1", fixture.peer_port());
  ASSERT_TRUE(opened.ok) << opened.error;

  // Advance the injected clock so the answer's activity stamp differs from the opening one.
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

// Dialling a peer whose flow has been closed opens a fresh flow, not the closed connection.
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
