//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "../helpers/core_fixture_helper.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"

using namespace athenasip;

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;

namespace {

const std::string kCert = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.cer";
const std::string kKey = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.key";

// RFC 3261 18, tested at the transport boundary: a real listener on a real socket and a client that writes
// exact bytes. The observable is a 401: a REGISTER with no credentials is challenged (10.3 step 6), so a
// challenge proves the bytes were framed into a message and answered down the same flow.

// A REGISTER with an optional body, to exercise Content-Length framing.
std::string register_request(const std::string& transport, const std::string& branch, const std::string& body = "") {
  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/" + transport + " 127.0.0.1:9;branch=" + branch + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: " + branch + "\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Contact: <sip:alice@127.0.0.1:9>\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  raw += "\r\n";
  raw += body;
  return raw;
}

// A request whose Content-Length is deliberately wrong, for the 18.3 datagram rules.
std::string register_claiming(const std::string& branch, std::size_t claimed, const std::string& body) {
  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 127.0.0.1:9;branch=" + branch + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: " + branch + "\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Contact: <sip:alice@127.0.0.1:9>\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "Content-Length: " + std::to_string(claimed) + "\r\n";
  raw += "\r\n";
  raw += body;
  return raw;
}

int responses_in(const std::string& text) {
  int count = 0;
  std::size_t at = 0;

  while ((at = text.find("SIP/2.0 ", at)) != std::string::npos) {
    ++count;
    at += 8;
  }

  return count;
}

// Reads whatever arrives within a deadline, without blocking. The reader owns the single outstanding read
// across calls: two overlapping reads on one socket deliver to either handler.
template <typename Stream>
struct Reader {
  net::io_context& io;
  Stream& stream;
  std::string collected;
  std::array<char, 65535> buffer{};
  bool armed = false;

  Reader(net::io_context& context, Stream& s) : io(context), stream(s) {}

  ~Reader() {
    boost::system::error_code ec;
    stream.lowest_layer().cancel(ec);
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
  }

  // Everything that arrived since the last call. Returns as soon as `wanted` responses are in.
  std::string take(std::chrono::milliseconds wait, int wanted = 1) {
    _wanted = wanted;
    _arm();
    io.restart();
    io.run_for(wait);

    auto out = collected;
    collected.clear();
    return out;
  }

 private:
  void _arm() {
    if (armed) return;
    armed = true;

    _start([this](const boost::system::error_code& ec, std::size_t length) {
      armed = false;
      if (ec) return;

      collected.append(buffer.data(), length);

      if (_wanted > 0 && responses_in(collected) >= _wanted) return io.stop();

      _arm();
    });
  }

  template <typename Handler>
  void _start(Handler handler) {
    if constexpr (std::is_same_v<Stream, udp::socket>) {
      stream.async_receive_from(net::buffer(buffer), _from, std::move(handler));
    } else {
      stream.async_read_some(net::buffer(buffer), std::move(handler));
    }
  }

  udp::endpoint _from;
  int _wanted = 1;
};

struct TcpFixture : CoreFixture {
  std::shared_ptr<servers::TCPServer> server;

  TcpFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::TCPServer>(logger, core, "127.0.0.1", 0);
    server->start();
  }

  ~TcpFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() const { return server->port(); }
};

struct TlsFixture : CoreFixture {
  std::shared_ptr<servers::TLSServer> server;

  TlsFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::TLSServer>(logger, core, "127.0.0.1", 0);
    EXPECT_TRUE(server->set_certificates(kCert, kKey));
    server->start();
  }

  ~TlsFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() const { return server->port(); }
};

struct UdpFixture : CoreFixture {
  std::shared_ptr<servers::UDPServer> server;

  UdpFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::UDPServer>(logger, core, "127.0.0.1", 0);
    server->start();
  }

  ~UdpFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() { return server->local_endpoint().port(); }
};

}  // namespace

// The baseline for the stream transports: bytes in, a challenge back.
TEST(TransportFramingTest, TcpCarriesARequestAndAnswersItDownTheSameConnection) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-tcp-plain")));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 3261 18.3: on a stream, Content-Length says where the body ends, and a message may arrive in any number
// of pieces.
TEST(TransportFramingTest, TcpReassemblesARequestSplitAcrossWrites) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto request = register_request("TCP", "z9hG4bK-tcp-split", "v=0\r\n");
  const auto cut = request.size() / 2;

  net::write(socket, net::buffer(request.substr(0, cut)));

  // Half a message is not answered.
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "");

  net::write(socket, net::buffer(request.substr(cut)));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// Two requests written together arrive as one read, and both are taken from it.
TEST(TransportFramingTest, TcpTakesTwoRequestsFromOneWrite) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto both = register_request("TCP", "z9hG4bK-tcp-first") + register_request("TCP", "z9hG4bK-tcp-second");
  net::write(socket, net::buffer(both));

  const auto answer = reader.take(std::chrono::seconds(3), 2);
  EXPECT_EQ(responses_in(answer), 2) << answer;
}

// Content-Length alone says where a body ends, so a body does not swallow the request after it.
TEST(TransportFramingTest, TcpUsesContentLengthToFindTheEndOfABody) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto both =
      register_request("TCP", "z9hG4bK-tcp-body", "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n") + register_request("TCP", "z9hG4bK-tcp-after");

  net::write(socket, net::buffer(both));

  const auto answer = reader.take(std::chrono::seconds(3), 2);
  EXPECT_EQ(responses_in(answer), 2) << answer;
}

// A message is not answered until its whole body has arrived.
TEST(TransportFramingTest, TcpWaitsForTheWholeBodyBeforeAnswering) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const std::string body = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
  const auto request = register_request("TCP", "z9hG4bK-tcp-partial", body);
  const auto headers_and_half = request.size() - (body.size() / 2);

  net::write(socket, net::buffer(request.substr(0, headers_and_half)));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "");

  net::write(socket, net::buffer(request.substr(headers_and_half)));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// A start line that does not parse must not throw out of the read handler: the node answers and keeps running.
TEST(TransportFramingTest, TcpSurvivesAMalformedStartLine) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(std::string("this is not a request line\r\n\r\n")));
  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-tcp-after-rubbish")));

  // 400 for the malformed line (RFC 3261 8.2.1), then a challenge for the request that followed it.
  const auto answer = reader.take(std::chrono::seconds(3), 2);

  EXPECT_NE(answer.find("SIP/2.0 400"), std::string::npos) << answer;
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 5626 4.4.1: on a stream a client's keep-alive is a double CRLF, and the server MUST answer with a single
// CRLF. A client that hears no pong treats its flow as failed and registers again.
TEST(TransportFramingTest, TcpAnswersAKeepAlivePingWithAPong) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(std::string("\r\n\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "\r\n");

  // The ping may arrive in two reads.
  net::write(socket, net::buffer(std::string("\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "") << "one CRLF is not a ping";
  net::write(socket, net::buffer(std::string("\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "\r\n");

  // A request after it is still a request.
  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-after-ping")));
  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

TEST(TransportFramingTest, TlsCarriesARequestOverACompletedHandshake) {
  TlsFixture f;

  net::io_context io;
  ssl::context context(ssl::context::tls_client);
  context.set_verify_mode(ssl::verify_none);

  ssl::stream<tcp::socket> stream(io, context);
  stream.next_layer().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  stream.handshake(ssl::stream_base::client);
  Reader<ssl::stream<tcp::socket>> reader(io, stream);

  net::write(stream, net::buffer(register_request("TLS", "z9hG4bK-tls-plain")));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// A failed handshake (a port scanner, plain SIP to the TLS port, a rejected certificate) is that connection's
// problem: the listener goes on accepting.
TEST(TransportFramingTest, TlsKeepsAcceptingAfterAFailedHandshake) {
  TlsFixture f;

  {
    net::io_context io;
    tcp::socket plain(io);
    plain.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
    net::write(plain, net::buffer(register_request("TCP", "z9hG4bK-not-tls")));
    std::array<char, 256> buffer{};
    boost::system::error_code ignored;
    plain.read_some(net::buffer(buffer), ignored);
  }

  net::io_context io;
  ssl::context context(ssl::context::tls_client);
  context.set_verify_mode(ssl::verify_none);

  ssl::stream<tcp::socket> stream(io, context);
  stream.next_layer().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));

  boost::system::error_code handshake;
  net::steady_timer deadline(io);
  bool done = false;
  stream.async_handshake(ssl::stream_base::client, [&](const boost::system::error_code& ec) {
    handshake = ec;
    done = true;
  });
  io.run_for(std::chrono::seconds(3));
  ASSERT_TRUE(done) << "the listener stopped accepting";
  ASSERT_FALSE(handshake) << handshake.message();

  Reader<ssl::stream<tcp::socket>> reader(io, stream);
  net::write(stream, net::buffer(register_request("TLS", "z9hG4bK-tls-after")));
  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// A connection that says nothing holds up nobody else: the handshake is per connection.
TEST(TransportFramingTest, ASilentTlsConnectionHoldsUpNobodyElse) {
  TlsFixture f;

  net::io_context silent_io;
  tcp::socket silent(silent_io);
  silent.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));

  net::io_context io;
  ssl::context context(ssl::context::tls_client);
  context.set_verify_mode(ssl::verify_none);

  ssl::stream<tcp::socket> stream(io, context);
  stream.next_layer().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));

  bool done = false;
  boost::system::error_code handshake;
  stream.async_handshake(ssl::stream_base::client, [&](const boost::system::error_code& ec) {
    handshake = ec;
    done = true;
  });
  io.run_for(std::chrono::seconds(3));
  ASSERT_TRUE(done) << "a silent connection held up the next one";
  EXPECT_FALSE(handshake) << handshake.message();
}

TEST(TransportFramingTest, UdpCarriesADatagramAndAnswersItToTheSourcePort) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-plain")), node);

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 3261 18.3: a datagram whose Content-Length exceeds its body MUST be discarded. No later packet completes
// it.
TEST(TransportFramingTest, UdpDiscardsADatagramThatDoesNotCarryTheBodyItClaims) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_claiming("z9hG4bK-udp-liar", 5000, "v=0\r\n")), node);

  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "");
}

// The discarded datagram must not take the next one as its body.
TEST(TransportFramingTest, ADiscardedDatagramDoesNotSwallowTheNextOne) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_claiming("z9hG4bK-udp-liar-2", 5000, "v=0\r\n")), node);
  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-good")), node);

  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_EQ(responses_in(answer), 1) << answer;
  EXPECT_NE(answer.find("z9hG4bK-udp-good"), std::string::npos) << answer;
}

// RFC 3261 18.3: a body longer than its Content-Length is truncated to that length. The extra bytes are not a
// second message.
TEST(TransportFramingTest, UdpTruncatesABodyLongerThanItsContentLength) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  auto datagram = register_claiming("z9hG4bK-udp-long", 5, "v=0\r\nand a great deal more that nobody asked for\r\n");
  socket.send_to(net::buffer(datagram), node);

  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_EQ(responses_in(answer), 1) << answer;
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// A datagram that is not SIP does not stop the listener.
TEST(TransportFramingTest, UdpKeepsListeningAfterADatagramThatIsNotSip) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(std::string("this is not a SIP message at all")), node);
  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-after-rubbish")), node);

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 5626 4.4.2: an outbound client keeps its UDP flow alive with STUN Binding requests to the SIP port, and
// the node answers each with the address it came from.
TEST(TransportFramingTest, UdpAnswersAStunKeepAliveWithWhereItCameFrom) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const auto local_port = socket.local_endpoint().port();

  std::string request("\x00\x01\x00\x00\x21\x12\xA4\x42", 8);
  request += "keepalive-01";
  socket.send_to(net::buffer(request), udp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));

  std::array<char, 512> buffer{};
  udp::endpoint from;
  std::size_t received = 0;
  socket.async_receive_from(net::buffer(buffer), from, [&received](const boost::system::error_code& ec, std::size_t length) {
    if (!ec) received = length;
  });
  io.run_for(std::chrono::seconds(2));

  ASSERT_EQ(received, 32u);
  const std::string response(buffer.data(), received);
  EXPECT_EQ(response.substr(0, 2), std::string("\x01\x01", 2));
  EXPECT_EQ(response.substr(8, 12), "keepalive-01");

  const auto port = static_cast<std::uint16_t>(((static_cast<unsigned char>(response[26]) << 8) | static_cast<unsigned char>(response[27])) ^ 0x2112);
  EXPECT_EQ(port, local_port);
}

// One flow per peer, not per datagram: the channel registry is keyed by transport, address and port, and
// responses go back to the request's source (RFC 3261 18.2.1).
TEST(TransportFramingTest, TwoDatagramsFromOnePeerShareOneFlow) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-one")), node);
  reader.take(std::chrono::seconds(2));

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-two")), node);
  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;

  const auto source = "udp://127.0.0.1:" + std::to_string(socket.local_endpoint().port());
  EXPECT_NE(f.on_strand([&]() { return f.core->channel_find(source); }), nullptr);
}
