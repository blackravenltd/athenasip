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
#include <string>
#include <thread>

#include "helpers/core_fixture_helper.h"

using namespace athenasip;

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

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

// UDP has no connection to open. A datagram to a host this node has never heard from has
// to leave by the listener's own socket so that the source port is the one the far end
// answers to, and that socket is the server's. Saying so beats pretending.
TEST(CoreConnectTest, RefusesToDialATransportWithNothingToDial) {
  ConnectFixture fixture;

  const auto udp = fixture.connect("udp", "127.0.0.1", 5060);
  EXPECT_FALSE(udp.ok);

  // Outbound TLS waits for the trust configuration the cluster CA brings, and guessing at
  // it would be worse than refusing.
  const auto tls = fixture.connect("tls", "127.0.0.1", 5061);
  EXPECT_FALSE(tls.ok);
}
