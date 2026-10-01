//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/resolver.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>

#include "../mocks/logger_mock.h"
#include "global_io_context.h"

using namespace athenasip;
using namespace athenasip::dns;
namespace net = boost::asio;
using udp = net::ip::udp;
using tcp = net::ip::tcp;

namespace {

std::vector<std::uint8_t> from_hex(const std::string& hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

// The real answer 1.1.1.1 gave for _sip._udp.iptel.org SRV; see message_test.
const char* kIptelSrv =
    "bc1181800001000100000000045f736970045f75647005697074656c036f72670000210001c00c002100010000025800150000001913c40373697005697074656c036f726700";

// A nameserver on loopback, on its own thread, doing whatever a test tells it to.
struct FakeNameserver {
  enum class Mode { Answer, Silent, WrongIdFirst, Truncate, ServFail, DropFirst };

  net::io_context io;
  udp::socket socket{io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
  tcp::acceptor acceptor{io};
  std::thread thread;

  Mode mode = Mode::Answer;
  std::vector<std::uint8_t> answer = from_hex(kIptelSrv);
  std::atomic<int> asked{0};
  std::atomic<int> asked_over_tcp{0};

  FakeNameserver() {
    // TCP on the same port number, which is where a resolver goes when UDP was truncated.
    acceptor.open(tcp::v4());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), socket.local_endpoint().port()));
    acceptor.listen();

    _receive();
    _accept();
    thread = std::thread([this]() { io.run(); });
  }

  ~FakeNameserver() {
    io.stop();
    if (thread.joinable()) thread.join();
  }

  udp::endpoint endpoint() const { return socket.local_endpoint(); }

 private:
  std::array<std::uint8_t, 2048> _buffer{};
  udp::endpoint _from;

  std::vector<std::uint8_t> _reply_to(const std::uint8_t* query, bool truncated) {
    auto reply = answer;
    reply[0] = query[0];
    reply[1] = query[1];
    if (truncated) reply[2] |= 0x02;
    return reply;
  }

  void _receive() {
    socket.async_receive_from(net::buffer(_buffer), _from, [this](const boost::system::error_code& ec, std::size_t) {
      if (ec) return;
      asked++;

      auto send = [this](std::vector<std::uint8_t> reply) {
        auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(reply));
        socket.async_send_to(net::buffer(*data), _from, [data](const boost::system::error_code&, std::size_t) {});
      };

      switch (mode) {
        case Mode::Answer:
          send(_reply_to(_buffer.data(), false));
          break;
        case Mode::Silent:
          break;
        case Mode::DropFirst:
          if (asked.load() > 1) send(_reply_to(_buffer.data(), false));
          break;
        case Mode::WrongIdFirst: {
          auto forged = _reply_to(_buffer.data(), false);
          forged[0] ^= 0xff;
          forged[53] = 0;  // and port 0 in the SRV, so taking it would show
          forged[54] = 0;
          send(forged);
          send(_reply_to(_buffer.data(), false));
          break;
        }
        case Mode::Truncate:
          send(_reply_to(_buffer.data(), true));
          break;
        case Mode::ServFail: {
          auto failed = _reply_to(_buffer.data(), false);
          failed[3] = static_cast<std::uint8_t>((failed[3] & 0xf0) | 0x02);
          send(failed);
          break;
        }
      }
      _receive();
    });
  }

  void _accept() {
    acceptor.async_accept([this](const boost::system::error_code& ec, tcp::socket peer) {
      if (ec) return;
      asked_over_tcp++;

      auto stream = std::make_shared<tcp::socket>(std::move(peer));
      auto length = std::make_shared<std::array<std::uint8_t, 2>>();

      net::async_read(*stream, net::buffer(*length), [this, stream, length](const boost::system::error_code& ec, std::size_t) {
        if (ec) return;
        auto query = std::make_shared<std::vector<std::uint8_t>>(((*length)[0] << 8) | (*length)[1]);

        net::async_read(*stream, net::buffer(*query), [this, stream, query](const boost::system::error_code& ec, std::size_t) {
          if (ec) return;
          auto reply = _reply_to(query->data(), false);
          auto framed = std::make_shared<std::vector<std::uint8_t>>();
          framed->push_back(static_cast<std::uint8_t>(reply.size() >> 8));
          framed->push_back(static_cast<std::uint8_t>(reply.size() & 0xff));
          framed->insert(framed->end(), reply.begin(), reply.end());
          net::async_write(*stream, net::buffer(*framed), [stream, framed](const boost::system::error_code&, std::size_t) {});
        });
      });
      _accept();
    });
  }
};

plugins::Result<std::vector<Record>> ask(const std::shared_ptr<UdpResolver>& resolver, const std::string& name, Type type) {
  std::promise<plugins::Result<std::vector<Record>>> promise;
  auto future = promise.get_future();

  auto executor = detail::get_global_io_context().get_executor();
  resolver->query(executor, name, type, [&promise](plugins::Result<std::vector<Record>> result) { promise.set_value(std::move(result)); });

  // The global io_context has a thread of its own, which is what the resolver runs on.
  if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    ADD_FAILURE() << "the resolver never answered";
    return plugins::Result<std::vector<Record>>::failure("never answered");
  }

  return future.get();
}

std::shared_ptr<UdpResolver> resolver_for(std::vector<udp::endpoint> servers, std::chrono::milliseconds timeout = std::chrono::milliseconds(300)) {
  return std::make_shared<UdpResolver>(std::make_shared<MockLogger>(), std::move(servers), timeout);
}

}  // namespace

TEST(DnsResolverTest, AnAnswerIsTheRecordsOfTheTypeAsked) {
  FakeNameserver server;

  const auto answer = ask(resolver_for({server.endpoint()}), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  ASSERT_EQ(answer.value.size(), 1u);
  EXPECT_EQ(answer.value[0].srv.target, "sip.iptel.org");
  EXPECT_EQ(answer.value[0].srv.port, 5060);
}

// RFC 5452: a reply whose id is not the one sent is not an answer, however well-formed. The
// resolver keeps listening and takes the real one.
TEST(DnsResolverTest, AReplyWithTheWrongIdIsIgnored) {
  FakeNameserver server;
  server.mode = FakeNameserver::Mode::WrongIdFirst;

  const auto answer = ask(resolver_for({server.endpoint()}), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  ASSERT_EQ(answer.value.size(), 1u);
  EXPECT_EQ(answer.value[0].srv.target, "sip.iptel.org");
  EXPECT_EQ(answer.value[0].srv.port, 5060) << "the forged reply was taken";
}

// resolv.conf(5): "attempts" defaults to 2. A datagram lost once is asked for again, or a
// machine with one nameserver loses a whole answer to one dropped packet - which is what a
// run against the real DNS did, and how this was found.
TEST(DnsResolverTest, ALostQueryIsAskedAgain) {
  FakeNameserver server;
  server.mode = FakeNameserver::Mode::DropFirst;

  const auto answer = ask(resolver_for({server.endpoint()}, std::chrono::milliseconds(200)), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  EXPECT_EQ(server.asked.load(), 2);
  EXPECT_EQ(answer.value.size(), 1u);
}

// RFC 1035 7.4 and RFC 2308: an answer is good for its TTL - 600 seconds in this one - and
// asking again inside it costs a call setup a round trip to the network for nothing.
TEST(DnsResolverTest, AnAnswerIsKeptForItsTtl) {
  FakeNameserver server;
  auto resolver = resolver_for({server.endpoint()});

  const auto first = ask(resolver, "_sip._udp.iptel.org", Type::SRV);
  const auto second = ask(resolver, "_SIP._udp.iptel.org.", Type::SRV);

  ASSERT_TRUE(first.ok);
  ASSERT_TRUE(second.ok);
  EXPECT_EQ(second.value.size(), 1u);
  EXPECT_EQ(server.asked.load(), 1) << "DNS names are case-insensitive and the trailing dot is the root (RFC 4343, RFC 1034)";
}

// A different question is a different answer.
TEST(DnsResolverTest, TheCacheIsByNameAndType) {
  FakeNameserver server;
  auto resolver = resolver_for({server.endpoint()});

  ask(resolver, "_sip._udp.iptel.org", Type::SRV);
  ask(resolver, "_sip._udp.iptel.org", Type::NAPTR);

  EXPECT_EQ(server.asked.load(), 2);
}

// Not hearing back is not an answer, and is not kept.
TEST(DnsResolverTest, NoAnswerIsNotKept) {
  FakeNameserver server;
  server.mode = FakeNameserver::Mode::Silent;
  auto resolver = resolver_for({server.endpoint()}, std::chrono::milliseconds(100));

  ask(resolver, "_sip._udp.iptel.org", Type::SRV);
  server.mode = FakeNameserver::Mode::Answer;
  const auto later = ask(resolver, "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(later.ok) << later.error;
  EXPECT_EQ(later.value.size(), 1u);
}

// A server that says nothing is a server to move on from, within the bound.
TEST(DnsResolverTest, ASilentServerIsPassedOverForTheNext) {
  FakeNameserver silent;
  silent.mode = FakeNameserver::Mode::Silent;
  FakeNameserver working;

  const auto answer = ask(resolver_for({silent.endpoint(), working.endpoint()}), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  EXPECT_EQ(silent.asked.load(), 1);
  EXPECT_EQ(answer.value.size(), 1u);
}

TEST(DnsResolverTest, AServerThatFailsIsPassedOverForTheNext) {
  FakeNameserver failing;
  failing.mode = FakeNameserver::Mode::ServFail;
  FakeNameserver working;

  const auto answer = ask(resolver_for({failing.endpoint(), working.endpoint()}), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  EXPECT_EQ(answer.value.size(), 1u);
}

// No answer from anybody is a failure, and it comes back in bounded time rather than when
// the operating system gives up.
TEST(DnsResolverTest, NoAnswerFromAnyServerIsAFailureInBoundedTime) {
  FakeNameserver one;
  one.mode = FakeNameserver::Mode::Silent;
  FakeNameserver two;
  two.mode = FakeNameserver::Mode::Silent;

  const auto started = std::chrono::steady_clock::now();
  const auto answer = ask(resolver_for({one.endpoint(), two.endpoint()}, std::chrono::milliseconds(200)), "_sip._udp.iptel.org", Type::SRV);

  EXPECT_FALSE(answer.ok);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

// RFC 1035 4.2.2 and RFC 7766: an answer that did not fit is asked for again over TCP.
TEST(DnsResolverTest, ATruncatedAnswerIsAskedForAgainOverTcp) {
  FakeNameserver server;
  server.mode = FakeNameserver::Mode::Truncate;

  const auto answer = ask(resolver_for({server.endpoint()}), "_sip._udp.iptel.org", Type::SRV);

  ASSERT_TRUE(answer.ok) << answer.error;
  EXPECT_EQ(server.asked_over_tcp.load(), 1);
  ASSERT_EQ(answer.value.size(), 1u);
  EXPECT_EQ(answer.value[0].srv.target, "sip.iptel.org");
}

TEST(DnsResolverTest, NoServersIsAFailureAtOnce) {
  const auto answer = ask(resolver_for({}), "_sip._udp.iptel.org", Type::SRV);
  EXPECT_FALSE(answer.ok);
}

// resolv.conf(5): nameserver lines, in order, and nothing else.
TEST(DnsResolverTest, NameserversAreReadFromResolvConf) {
  const auto path = std::string(testing::TempDir()) + "/athena-resolv.conf";
  {
    std::ofstream file(path);
    file << "# a comment\n"
         << "domain example.com\n"
         << "nameserver 192.0.2.53\n"
         << "search example.com\n"
         << "nameserver 2001:db8::53 ; trailing comment\n"
         << "nameserver not-an-address\n"
         << "options ndots:2\n";
  }

  const auto servers = UdpResolver::servers_from(path);

  ASSERT_EQ(servers.size(), 2u);
  EXPECT_EQ(servers[0].address().to_string(), "192.0.2.53");
  EXPECT_EQ(servers[0].port(), 53);
  EXPECT_EQ(servers[1].address().to_string(), "2001:db8::53");
}

TEST(DnsResolverTest, NoResolvConfIsNoServers) { EXPECT_TRUE(UdpResolver::servers_from("/nonexistent/resolv.conf").empty()); }
